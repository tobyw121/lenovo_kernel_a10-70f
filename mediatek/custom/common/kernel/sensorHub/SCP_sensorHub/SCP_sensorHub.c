/* SCP sensor hub driver
 *
 *
 * This software program is licensed subject to the GNU General Public License
 * (GPL).Version 2,June 1991, available at http://www.fsf.org/copyleft/gpl.html

 * (C) Copyright 2011 Bosch Sensortec GmbH
 * All Rights Reserved
 */

#include <linux/interrupt.h>
#include <linux/slab.h>
#include <linux/irq.h>
#include <linux/miscdevice.h>
#include <asm/uaccess.h>
#include <linux/delay.h>
#include <linux/input.h>
#include <linux/workqueue.h>
#include <linux/kobject.h>
#include <linux/earlysuspend.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <asm/atomic.h>

//#include <mach/mt_devs.h>
#include <mach/mt_typedefs.h>
#include <mach/mt_gpio.h>
#include <mach/mt_pm_ldo.h>

#include "step_counter.h"
#include <linux/batch.h>
#include <mach/md32_ipi.h>

#define POWER_NONE_MACRO MT65XX_POWER_NONE

#include <cust_sensorHub.h>
#include <linux/hwmsensor.h>
#include <linux/hwmsen_dev.h>
#include <linux/sensors_io.h>
#include "SCP_sensorHub.h"
#include <linux/hwmsen_helper.h>
#include <mach/mt_clkmgr.h>
/*----------------------------------------------------------------------------*/
#define DEBUG 1
//#define SENSORHUB_UT
/*----------------------------------------------------------------------------*/
//#define CONFIG_SCP_sensorHub_LOWPASS   /*apply low pass filter on output*/       
#define SW_CALIBRATION
/*----------------------------------------------------------------------------*/
#define SCP_sensorHub_AXIS_X          0
#define SCP_sensorHub_AXIS_Y          1
#define SCP_sensorHub_AXIS_Z          2
#define SCP_sensorHub_AXES_NUM        3
#define SCP_sensorHub_DATA_LEN        6
#define SCP_sensorHub_DEV_NAME        "SCP_sensorHub"

/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_probe(void);
static int SCP_sensorHub_remove(void);
//static int SCP_sensorHub_suspend(struct platform_device *dev, pm_message_t state);
//static int SCP_sensorHub_resume(struct platform_device *dev);

static int SCP_sensorHub_local_init(void);
#ifdef CUSTOM_KERNEL_STEP_COUNTER
static void SCP_sd_work(struct work_struct *work);
static void SCP_sm_work(struct work_struct *work);
static int SCP_sensorHub_step_counter_init(void);
static int SCP_sensorHub_step_counter_uninit(void);
#endif //#ifdef CUSTOM_KERNEL_STEP_COUNTER
/*----------------------------------------------------------------------------*/
typedef enum {
    SCP_TRC_FUN =   0x01,
    SCP_TRC_IPI =   0x02,
    SCP_TRC_BATCH = 0x04,
} SCP_TRC;
/*----------------------------------------------------------------------------*/
SCP_sensorHub_handler sensor_handler[ID_SENSOR_MAX_HANDLE+1];
/*----------------------------------------------------------------------------*/
#define C_MAX_FIR_LENGTH (32)
//#define USE_EARLY_SUSPEND
static DEFINE_MUTEX(SCP_sensorHub_op_mutex);
static DEFINE_MUTEX(SCP_sensorHub_req_mutex);
static DECLARE_WAIT_QUEUE_HEAD(SCP_sensorHub_req_wq);

static int SCP_sensorHub_init_flag =-1; // 0<==>OK -1 <==> fail

static struct batch_init_info SCP_sensorHub_init_info = {
		.name = "SCP_sensorHub",
		.init = SCP_sensorHub_local_init,
		.uninit = SCP_sensorHub_remove,
		.platform_diver_addr = NULL,
};

#ifdef CUSTOM_KERNEL_STEP_COUNTER
static struct step_c_init_info SCP_pedometer_init_info = {
		.name = "SCP_pedometer",
		.init = SCP_sensorHub_step_counter_init,
		.uninit = SCP_sensorHub_step_counter_uninit,
};
#endif //#ifdef CUSTOM_KERNEL_STEP_COUNTER

/*----------------------------------------------------------------------------*/
struct data_filter {
    s16 raw[C_MAX_FIR_LENGTH][SCP_sensorHub_AXES_NUM];
    int sum[SCP_sensorHub_AXES_NUM];
    int num;
    int idx;
};
/*----------------------------------------------------------------------------*/
struct SCP_sensorHub_data {
    struct sensorHub_hw *hw;
    struct work_struct	    ipi_work;
    struct work_struct      fifo_full_work;
    struct work_struct	    sd_work; //step detect work
    struct work_struct	    sm_work; //significant motion work
    struct timer_list       timer;
    
    /*misc*/
    atomic_t                trace;
    atomic_t                suspend;
	atomic_t				filter;
    s16                     cali_sw[SCP_sensorHub_AXES_NUM+1];
    atomic_t                wait_rsp;
    atomic_t                ipi_handler_running;

    /*data*/
    s8                      offset[SCP_sensorHub_AXES_NUM+1];  /*+1: for 4-byte alignment*/
    s16                     data[SCP_sensorHub_AXES_NUM+1];
    volatile struct sensorFIFO * volatile SCP_sensorFIFO;
    dma_addr_t              mapping;

#if defined(CONFIG_SCP_sensorHub_LOWPASS)
    atomic_t                firlen;
    atomic_t                fir_en;
    struct data_filter      fir;
#endif 
    /*early suspend*/
#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(USE_EARLY_SUSPEND)
    struct early_suspend    early_drv;
#endif     
};
/*----------------------------------------------------------------------------*/
static struct SCP_sensorHub_data *obj_data = NULL;
static SCP_SENSOR_HUB_DATA_P userData = NULL;
static uint *userDataLen = NULL;
/*----------------------------------------------------------------------------*/
#define SCP_TAG                  "[sensorHub] "
#define SCP_FUN(f)               printk(KERN_ERR SCP_TAG"%s\n", __FUNCTION__)
#define SCP_ERR(fmt, args...)    printk(KERN_ERR SCP_TAG"%s %d : "fmt, __FUNCTION__, __LINE__, ##args)
#define SCP_LOG(fmt, args...)    printk(KERN_ERR SCP_TAG fmt, ##args)
/*--------------------SCP_sensorHub power control function----------------------------------*/
static void SCP_sensorHub_power(struct sensorHub_hw *hw, unsigned int on) 
{
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_init_client(void) //call by init done workqueue
{
    struct SCP_sensorHub_data *obj = obj_data;
    SCP_SENSOR_HUB_DATA data;
    unsigned int len = 0;

    SCP_FUN();

    enable_clock(MT_CG_INFRA_APDMA, "sensorHub");
    obj->mapping = dma_map_single(NULL, (void *)obj->SCP_sensorFIFO, obj->SCP_sensorFIFO->FIFOSize, DMA_BIDIRECTIONAL);//(virt_to_phys(obj->SCP_sensorFIFO));
    dma_sync_single_for_device(NULL, obj->mapping, obj->SCP_sensorFIFO->FIFOSize, DMA_TO_DEVICE);

    data.set_config_req.sensorType = 0;
    data.set_config_req.action = SENSOR_HUB_SET_CONFIG;
    data.set_config_req.bufferBase = (struct sensorFIFO *)obj->mapping;
    SCP_ERR("data.set_config_req.bufferBase = %p\n", data.set_config_req.bufferBase);
    data.set_config_req.bufferSize = obj->SCP_sensorFIFO->FIFOSize;
    len = sizeof(data.set_config_req);

    SCP_sensorHub_req_send(&data, &len, 1);

	return SCP_SENSOR_HUB_SUCCESS;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_ReadChipInfo(char *buf, int bufsize)
{
	if((NULL == buf)||(bufsize<=30))
	{
		return -1;
	}

	sprintf(buf, "SCP_sensorHub Chip");
	return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_ReadSensorData(int handle, hwm_sensor_data *sensorData)
{
	struct SCP_sensorHub_data *obj = obj_data;
    char *pStart, *pEnd, *pNext;
    struct SCP_sensorData *curData;
    char *rp, *wp;

#if 1//def SENSORHUB_UT
    //SCP_FUN();
#endif

	if(NULL == sensorData)
	{
		return -1;
	}

    //data.get_data_req.sensorType = handle;
    //data.get_data_req.action = SENSOR_HUB_GET_DATA;
    //len = sizeof(data.get_data_req);

    //if (0 != (err = SCP_sensorHub_req_send(&data, &len, 1)))
	//{
	//	SCP_ERR("SCP_sensorHub_req_send error: ret value=%d\n", err);
	//	return -3;
	//}
	//else
	//{
        
        dma_sync_single_for_cpu(NULL, obj->mapping, obj->SCP_sensorFIFO->FIFOSize, DMA_FROM_DEVICE);
        pStart = (char *)obj->SCP_sensorFIFO + offsetof(struct sensorFIFO, data);//sizeof(obj->SCP_sensorFIFO);
        pEnd = (char *)pStart + obj->SCP_sensorFIFO->FIFOSize;
        rp = pStart + (int)obj->SCP_sensorFIFO->rp;
        wp = pStart + (int)obj->SCP_sensorFIFO->wp;

        //SCP_LOG("FIFO virt_to_phys(obj->SCP_sensorFIFO) = %p\n", (void *)virt_to_phys(obj->SCP_sensorFIFO));
        //SCP_LOG("FIFO obj->SCP_sensorFIFO = %p, pStart = %p, pEnd = %p\n", obj->SCP_sensorFIFO, pStart, pEnd);
        //SCP_LOG("FIFO rp = %p, wp = %p, size = %d\n", rp, wp, obj->SCP_sensorFIFO->FIFOSize);
        //SCP_LOG("FIFO sensorType = %d, dataLength = %d\n", obj->SCP_sensorFIFO->data[0].sensorType,
        //        obj->SCP_sensorFIFO->data[0].dataLength);
        
        if (rp < pStart || pEnd <= rp)
        {
            SCP_ERR("FIFO rp invalid : %p, %p, %p\n", pStart, pEnd, rp);
            return -4;
        }

        if (wp < pStart || pEnd <= wp)
        {
            SCP_ERR("FIFO wp invalid : %p, %p, %p\n", pStart, pEnd, wp);
            return -5;
        }

        if (rp == wp)
        {
            //obj->SCP_sensorFIFO->rp += 1;
            //obj->SCP_sensorFIFO->wp += 1;
            SCP_ERR("FIFO empty\n");
            return -6;
        }
        
	    //while(rp != wp)
	    if (rp != wp)
	    {
            pNext = rp + offsetof(struct SCP_sensorData, data) + ((struct SCP_sensorData*)rp)->dataLength;
            pNext = (char *)((((unsigned long)pNext + 3) >> 2 ) << 2);
            //SCP_LOG("dataLength = %d, pNext = %p, rp = %p, wp = %p\n", ((struct SCP_sensorData*)rp)->dataLength, pNext, rp, wp);
            //SCP_LOG("[0] = %d, [1] = %d, [2] = %d\n", ((struct SCP_sensorData*)rp)->data[0], ((struct SCP_sensorData*)rp)->data[1], ((struct SCP_sensorData*)rp)->data[2]);
            
            if(!(curData = kzalloc(pNext - rp, GFP_KERNEL)))
            {
                SCP_ERR("Allocate curData fail\n");
                return -7;
            }
            
            if (pNext < pEnd)
            {
                memcpy(curData, rp, pNext - rp);
                rp = pNext;
            }
            else
            {
                memcpy(curData, rp, pEnd - rp);
                memcpy((char *)curData + (int)pEnd - (int)rp, pStart, (int)pNext - (int)pEnd);
                //SCP_LOG("!pNext < pEnd : pEnd - rp = %d\n", pEnd - rp);
                //SCP_LOG("!pNext < pEnd : curData = %p, (char *)&curData + (int)pEnd - (int)rp = %p\n", curData, (char *)curData + (int)pEnd - (int)rp);
                //SCP_LOG("!pNext < pEnd : (int)pNext - (int)pEnd = %d\n", (int)pNext - (int)pEnd);

                rp = pStart + (int)pNext - (int)pEnd;
            }

            //SCP_LOG("rp = %p, curData.sensorType = %d\n", rp, curData.sensorType);

            sensorData->sensor = curData->sensorType;
            sensorData->value_divide = 1000; //need to check
            sensorData->status = SENSOR_STATUS_ACCURACY_MEDIUM;
            sensorData->time = (((int64_t)curData->timeStampH) << 32) | curData->timeStampL;
            //for (i=0;i<curData.dataLength;i++)
            {
                sensorData->values[0] = curData->data[0];
                sensorData->values[1] = curData->data[1];
                sensorData->values[2] = curData->data[2];
            }

            obj->SCP_sensorFIFO->rp = (struct SCP_sensorData*)(rp - pStart);

            kfree(curData);

            dma_sync_single_for_device(NULL, obj->mapping, obj->SCP_sensorFIFO->FIFOSize, DMA_TO_DEVICE);
	    }

        
	//}
	
	return 0;
}
/*----------------------------------------------------------------------------*/
static ssize_t show_chipinfo_value(struct device_driver *ddri, char *buf)
{
	char strbuf[SCP_SENSOR_HUB_TEMP_BUFSIZE];
	
	SCP_sensorHub_ReadChipInfo(strbuf, SCP_SENSOR_HUB_TEMP_BUFSIZE);
	return snprintf(buf, PAGE_SIZE, "%s\n", strbuf);        
}
/*----------------------------------------------------------------------------*/
static ssize_t show_trace_value(struct device_driver *ddri, char *buf)
{
	ssize_t res;
	struct SCP_sensorHub_data *obj = obj_data;
	if (obj == NULL)
	{
		SCP_ERR("SCP_sensorHub_data obj is null!!\n");
		return 0;
	}
	
	res = snprintf(buf, PAGE_SIZE, "0x%04X\n", atomic_read(&obj->trace));     
	return res;    
}
/*----------------------------------------------------------------------------*/
static ssize_t store_trace_value(struct device_driver *ddri, const char *buf, size_t count)
{
	struct SCP_sensorHub_data *obj = obj_data;
	int trace;
	if (obj == NULL)
	{
		SCP_ERR("SCP_sensorHub_data obj is null!!\n");
		return 0;
	}
	
	if(1 == sscanf(buf, "0x%x", &trace))
	{
		atomic_set(&obj->trace, trace);
	}	
	else
	{
		SCP_ERR("invalid content: '%s', length = %d\n", buf, (int)count);
	}
	
	return count;    
}
/*----------------------------------------------------------------------------*/
static DRIVER_ATTR(chipinfo,   S_IWUSR | S_IRUGO, show_chipinfo_value,      NULL);
static DRIVER_ATTR(trace,      S_IWUSR | S_IRUGO, show_trace_value,         store_trace_value);
/*----------------------------------------------------------------------------*/
static struct driver_attribute *SCP_sensorHub_attr_list[] = {
	&driver_attr_chipinfo,     /*chip information*/
	&driver_attr_trace,        /*trace log*/
};
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_create_attr(struct device_driver *driver) 
{
	int idx, err = 0;
	int num = (int)(sizeof(SCP_sensorHub_attr_list)/sizeof(SCP_sensorHub_attr_list[0]));
	if (driver == NULL)
	{
		return -EINVAL;
	}

	for(idx = 0; idx < num; idx++)
	{
		if((err = driver_create_file(driver, SCP_sensorHub_attr_list[idx])))
		{            
			SCP_ERR("driver_create_file (%s) = %d\n", SCP_sensorHub_attr_list[idx]->attr.name, err);
			break;
		}
	}    
	return err;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_delete_attr(struct device_driver *driver)
{
	int idx ,err = 0;
	int num = (int)(sizeof(SCP_sensorHub_attr_list)/sizeof(SCP_sensorHub_attr_list[0]));

	if(driver == NULL)
	{
		return -EINVAL;
	}
	

	for(idx = 0; idx < num; idx++)
	{
		driver_remove_file(driver, SCP_sensorHub_attr_list[idx]);
	}
	

	return err;
}
/****************************************************************************** 
 * Function Configuration
******************************************************************************/
static int SCP_sensorHub_open(struct inode *inode, struct file *file)
{
	file->private_data = obj_data;

	if(file->private_data == NULL)
	{
		SCP_ERR("null pointer!!\n");
		return -EINVAL;
	}
	return nonseekable_open(inode, file);
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_release(struct inode *inode, struct file *file)
{
	file->private_data = NULL;
	return 0;
}
/*----------------------------------------------------------------------------*/
static long SCP_sensorHub_unlocked_ioctl(struct file *file, unsigned int cmd,unsigned long arg)       
{
	char strbuf[SCP_SENSOR_HUB_TEMP_BUFSIZE];
	void __user *data;
	long err = 0;

#ifdef SENSORHUB_UT
    SCP_FUN();
#endif

	if(_IOC_DIR(cmd) & _IOC_READ)
	{
		err = !access_ok(VERIFY_WRITE, (void __user *)arg, _IOC_SIZE(cmd));
	}
	else if(_IOC_DIR(cmd) & _IOC_WRITE)
	{
		err = !access_ok(VERIFY_READ, (void __user *)arg, _IOC_SIZE(cmd));
	}

	if(err)
	{
		SCP_ERR("access error: %08X, (%2d, %2d)\n", cmd, _IOC_DIR(cmd), _IOC_SIZE(cmd));
		return -EFAULT;
	}

	switch(cmd)
	{
		case GSENSOR_IOCTL_INIT:
			SCP_sensorHub_init_client();
			break;

		case GSENSOR_IOCTL_READ_CHIPINFO:
			data = (void __user *) arg;
			if(data == NULL)
			{
				err = -EINVAL;
				break;
			}
			
			SCP_sensorHub_ReadChipInfo(strbuf, SCP_SENSOR_HUB_TEMP_BUFSIZE);
			if(copy_to_user(data, strbuf, strlen(strbuf)+1))
			{
				err = -EFAULT;
				break;
			}
			break;

		case GSENSOR_IOCTL_READ_SENSORDATA:
			err = -EINVAL;
			break;

		case GSENSOR_IOCTL_READ_GAIN:
			err = -EINVAL; 
			break;

		case GSENSOR_IOCTL_READ_RAW_DATA:
			err = -EFAULT;
			break;	  

		case GSENSOR_IOCTL_SET_CALI:
			err = -EINVAL;
			break;

		case GSENSOR_IOCTL_CLR_CALI:
			err = -EINVAL;
			break;

		case GSENSOR_IOCTL_GET_CALI:
			err = -EINVAL;
			break;
		

		default:
			SCP_ERR("unknown IOCTL: 0x%08x\n", cmd);
			err = -ENOIOCTLCMD;
			break;
			
	}

	return err;
}


/*----------------------------------------------------------------------------*/
static struct file_operations SCP_sensorHub_fops = {
	//.owner = THIS_MODULE,
	.open = SCP_sensorHub_open,
	.release = SCP_sensorHub_release,
	.unlocked_ioctl = SCP_sensorHub_unlocked_ioctl,
};
/*----------------------------------------------------------------------------*/
static struct miscdevice SCP_sensorHub_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "SCP_sensorHub",
	.fops = &SCP_sensorHub_fops,
};
/*----------------------------------------------------------------------------*/
#if !defined(CONFIG_HAS_EARLYSUSPEND) || !defined(USE_EARLY_SUSPEND)
/*----------------------------------------------------------------------------*/
#if 0
static int SCP_sensorHub_suspend(struct platform_device *dev, pm_message_t state) 
{
	return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_resume(struct platform_device *dev)
{
	return 0;
}
/*----------------------------------------------------------------------------*/
#endif
#else //#if !defined(CONFIG_HAS_EARLYSUSPEND) || !defined(USE_EARLY_SUSPEND)
/*----------------------------------------------------------------------------*/
static void SCP_sensorHub_early_suspend(struct early_suspend *h) 
{
}
/*----------------------------------------------------------------------------*/
static void SCP_sensorHub_late_resume(struct early_suspend *h)
{
}
/*----------------------------------------------------------------------------*/
#endif //#if !defined(CONFIG_HAS_EARLYSUSPEND) || !defined(USE_EARLY_SUSPEND)
/*----------------------------------------------------------------------------*/
int SCP_sensorHub_req_send(SCP_SENSOR_HUB_DATA_P data, uint *len, unsigned int wait)
{
    ipi_status status;

    if (SCP_TRC_IPI & atomic_read(&(obj_data->trace)))
        SCP_ERR("len = %d, type = %d, action = %d\n", *len, data->req.sensorType, data->req.action);

    if (*len > 48)
    {
        SCP_ERR("!!\n");
        return -1;
    }

    if (in_interrupt())
    {
        SCP_ERR("Can't do %s in interrupt context!!\n", __FUNCTION__);
        return -1;
    }
    
    if (ID_SENSOR_MAX_HANDLE < data->rsp.sensorType)
    {
        SCP_ERR("SCP_sensorHub_IPI_handler invalid sensor type %d\n", data->rsp.sensorType);
        return -1;
    }
    else
    {
        mutex_lock(&SCP_sensorHub_req_mutex);
        
        userData = data;
        userDataLen = len;
        
        switch(data->req.action)
        {
            case SENSOR_HUB_ACTIVATE:
                break;
            case SENSOR_HUB_SET_DELAY:
                break;
            case SENSOR_HUB_GET_DATA:
                break;
            case SENSOR_HUB_BATCH:
                break;
            case SENSOR_HUB_SET_CONFIG:
                break;
            case SENSOR_HUB_SET_CUST:
                break;
            default:
                break;
        }

        if (1 == wait)
        {
            if(atomic_read(&(obj_data->wait_rsp)) == 1)
    		{
    			SCP_ERR("SCP_sensorHub_req_send reentry\n");
    		}
            atomic_set(&(obj_data->wait_rsp), 1);
        }

        do
        {
            status = md32_ipi_send(IPI_SENSOR, data, *len, wait);
            if (ERROR == status)
            {
                SCP_ERR("md32_ipi_send ERROR\n");
                mutex_unlock(&SCP_sensorHub_req_mutex);
                return -1;
            }
        }
        while (BUSY == status);
        if (SCP_TRC_IPI & atomic_read(&(obj_data->trace)))
            SCP_ERR("md32_ipi_send DONE\n");
        mod_timer(&obj_data->timer, jiffies + 3*HZ);
        wait_event_interruptible(SCP_sensorHub_req_wq, (atomic_read(&(obj_data->wait_rsp)) == 0));
        del_timer_sync(&obj_data->timer);
        mutex_unlock(&SCP_sensorHub_req_mutex);
    }

    if (SCP_TRC_IPI & atomic_read(&(obj_data->trace)))
        SCP_ERR("SCP_sensorHub_req_send end\n");
    
    return 0;
}
/*----------------------------------------------------------------------------*/
int SCP_sensorHub_rsp_registration(int sensor, SCP_sensorHub_handler handler)
{
#ifdef SENSORHUB_UT
    SCP_FUN();
#endif
    
    if (ID_SENSOR_MAX_HANDLE < sensor)
    {
        SCP_ERR("SCP_sensorHub_rsp_registration invalid sensor %d\n", sensor);
    }

    if (NULL == handler)
    {
        SCP_ERR("SCP_sensorHub_rsp_registration null handler\n");
    }
    
    sensor_handler[sensor] = handler;
    
    return 0;
}
/*----------------------------------------------------------------------------*/
static void SCP_ipi_work(struct work_struct *work)
{
#ifdef SENSORHUB_UT
    SCP_FUN();
#endif

	SCP_sensorHub_init_client();
}
/*----------------------------------------------------------------------------*/
static void SCP_fifo_full_work(struct work_struct *work)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    batch_notify(TYPE_BATCHFULL);
}
/*----------------------------------------------------------------------------*/
static void SCP_sensorHub_req_send_timeout(unsigned long data)
{
	if(atomic_read(&(obj_data->wait_rsp)) == 1)
	{
        SCP_FUN();
        atomic_set(&(obj_data->wait_rsp), 0);
        wake_up(&SCP_sensorHub_req_wq);
	}
}
/*----------------------------------------------------------------------------*/
static void SCP_sensorHub_IPI_handler(int id, void *data, unsigned int len)
{
    struct SCP_sensorHub_data *obj = obj_data;
    SCP_SENSOR_HUB_DATA_P rsp = (SCP_SENSOR_HUB_DATA_P)data;
    bool wake_up_req = false;
    bool do_registed_handler = false;
    static int first_init_done = 0;

#ifdef SENSORHUB_UT
    SCP_FUN();
#endif

    if (SCP_TRC_IPI & atomic_read(&(obj_data->trace)))
        SCP_ERR("len = %d, type = %d, action = %d, errCode = %d\n", len, rsp->rsp.sensorType, rsp->rsp.action, rsp->rsp.errCode);

    if (len > 48)
    {
        SCP_ERR("SCP_sensorHub_IPI_handler len=%d error\n", len);
        return;
    }
    else
    {
        switch(rsp->rsp.action)
        {
            case SENSOR_HUB_ACTIVATE:
            case SENSOR_HUB_SET_DELAY:
            case SENSOR_HUB_GET_DATA:
            case SENSOR_HUB_BATCH:
            case SENSOR_HUB_SET_CONFIG:
            case SENSOR_HUB_SET_CUST:
                wake_up_req = true;
                break;
            case SENSOR_HUB_NOTIFY:
                switch(rsp->notify_rsp.event)
                {
                    case SCP_INIT_DONE:
                        if (0 == first_init_done)
                        {
                            schedule_work(&obj->ipi_work);
                            first_init_done = 1;
                        }
                        do_registed_handler = true;
                        break;
                    case SCP_FIFO_FULL:
                        schedule_work(&obj->fifo_full_work);
                        break;
                    case SCP_NOTIFY:
                        do_registed_handler = true;
                        break;
                    default:
                        break;
                }
                break;
            default:
                SCP_ERR("SCP_sensorHub_IPI_handler unknow action=%d error\n", rsp->rsp.action);
                return;
        }

        if (ID_SENSOR_MAX_HANDLE < rsp->rsp.sensorType)
        {
            SCP_ERR("SCP_sensorHub_IPI_handler invalid sensor type %d\n", rsp->rsp.sensorType);
            return;
        }
        else if (true == do_registed_handler)
        {
            if (NULL != sensor_handler[rsp->rsp.sensorType])
            {
                sensor_handler[rsp->rsp.sensorType](data, len);
            }
        }

        if(atomic_read(&(obj_data->wait_rsp)) == 1 && true == wake_up_req)
        {
            if (NULL == userData || NULL == userDataLen)
            {
                SCP_ERR("SCP_sensorHub_IPI_handler null pointer\n");
            }
            else
            {
                if (userData->req.sensorType != rsp->rsp.sensorType)
                    SCP_ERR("SCP_sensorHub_IPI_handler sensor type %d != %d\n", userData->req.sensorType, rsp->rsp.sensorType);
                if (userData->req.action != rsp->rsp.action)
                    SCP_ERR("SCP_sensorHub_IPI_handler action %d != %d\n", userData->req.action, rsp->rsp.action);
                memcpy(userData, rsp, len);
                *userDataLen = len;
            }
            atomic_set(&(obj_data->wait_rsp), 0);
            wake_up(&SCP_sensorHub_req_wq);
        }
    }
}
/*----------------------------------------------------------------------------*/
int SCP_sensorHub_enable_hw_batch(int handle, int enable, long long samplingPeriodNs,long long maxBatchReportLatencyNs)
{
    SCP_SENSOR_HUB_DATA req;
    int len;
    int err = 0;

    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    req.batch_req.sensorType = handle;
    req.batch_req.action = SENSOR_HUB_BATCH;
    req.batch_req.flag = 2;
    req.batch_req.period_ms = (unsigned int)samplingPeriodNs;
    req.batch_req.timeout_ms = (unsigned int)maxBatchReportLatencyNs;
    len = sizeof(req.batch_req);
    err = SCP_sensorHub_req_send(&req, &len, 1);
    if (err)
    {
        SCP_ERR("SCP_sensorHub_req_send fail!\n");
    }

    return err;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_flush(int handle)
{
    return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_get_data(int handle, hwm_sensor_data *sensorData)
{
#ifdef SENSORHUB_UT
    SCP_FUN();
#endif

	SCP_sensorHub_ReadSensorData(handle, sensorData);

	return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_get_fifo_status(int *dataLen, int *status, char *reserved)
{
    struct SCP_sensorHub_data *obj = obj_data;
    int err = 0;
    SCP_SENSOR_HUB_DATA data;
    char *pStart, *pEnd, *pNext;
    unsigned int len = 0;
    char *rp, *wp;

    *dataLen = 0;
    *status = 1;

    data.get_data_req.sensorType = 0;
    data.get_data_req.action = SENSOR_HUB_GET_DATA;
    len = sizeof(data.get_data_req);

    if (0 != (err = SCP_sensorHub_req_send(&data, &len, 1)))
	{
		SCP_ERR("SCP_sensorHub_req_send error: ret value=%d\n", err);
		return -3;
	}
	else
	{
        
        dma_sync_single_for_cpu(NULL, obj->mapping, obj->SCP_sensorFIFO->FIFOSize, DMA_FROM_DEVICE);
        pStart = (char *)obj->SCP_sensorFIFO + offsetof(struct sensorFIFO, data);//sizeof(obj->SCP_sensorFIFO);
        pEnd = (char *)pStart + obj->SCP_sensorFIFO->FIFOSize;
        rp = pStart + (int)obj->SCP_sensorFIFO->rp;
        wp = pStart + (int)obj->SCP_sensorFIFO->wp;

        if (SCP_TRC_BATCH & atomic_read(&(obj_data->trace)))
        {
            //SCP_LOG("FIFO virt_to_phys(obj->SCP_sensorFIFO) = %p\n", (void *)virt_to_phys(obj->SCP_sensorFIFO));
            //SCP_LOG("FIFO obj->SCP_sensorFIFO = %p, pStart = %p, pEnd = %p\n", obj->SCP_sensorFIFO, pStart, pEnd);
            SCP_LOG("FIFO rp = %p, wp = %p\n", rp, wp);
            //SCP_LOG("FIFO sensorType = %d, dataLength = %d\n", obj->SCP_sensorFIFO->data[0].sensorType,
            //        obj->SCP_sensorFIFO->data[0].dataLength);
        }
        
        if (rp < pStart || pEnd <= rp)
        {
            SCP_ERR("FIFO rp invalid : %p, %p, %p\n", pStart, pEnd, rp);
            return -4;
        }

        if (wp < pStart || pEnd <= wp)
        {
            SCP_ERR("FIFO wp invalid : %p, %p, %p\n", pStart, pEnd, wp);
            return -5;
        }

        if (rp == wp)
        {
            SCP_ERR("FIFO empty\n");
            return -6;
        }
        
	    while(rp != wp)
	    {
            pNext = rp + offsetof(struct SCP_sensorData, data) + ((struct SCP_sensorData*)rp)->dataLength;
            pNext = (char *)((((unsigned long)pNext + 3) >> 2 ) << 2);
            //SCP_LOG("((struct SCP_sensorData*)rp)->dataLength = %d, pNext = %p, rp = %p\n", ((struct SCP_sensorData*)rp)->dataLength, pNext, rp);
            
            if (pNext < pEnd)
            {
                rp = pNext;
            }
            else
            {
                rp = pStart + (int)pNext - (int)pEnd;
            }
            (*dataLen)++;
	    }

        //obj->SCP_sensorFIFO->rp = (struct SCP_sensorData*)(rp - pStart);
        dma_sync_single_for_device(NULL, obj->mapping, obj->SCP_sensorFIFO->FIFOSize, DMA_TO_DEVICE);
	}

    if (SCP_TRC_BATCH & atomic_read(&(obj_data->trace)))
        SCP_ERR("SCP_sensorHub_get_fifo_status dataLen = %d, status = %d\n", *dataLen, *status);
    
    //*len = 1;
    //*status = 1;

    return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_probe(/*struct platform_device *pdev*/)
{
	struct SCP_sensorHub_data *obj;
	int err = 0;
	struct batch_control_path ctl={0};
	struct batch_data_path data={0};
	SCP_FUN();

	if(!(obj = kzalloc(sizeof(*obj), GFP_KERNEL)))
	{
        SCP_ERR("Allocate SCP_sensorHub_data fail\n");
		err = -ENOMEM;
		goto exit;
	}
	
	memset(obj, 0, sizeof(struct SCP_sensorHub_data));

    if(!(obj->SCP_sensorFIFO = kzalloc(SCP_SENSOR_HUB_FIFO_SIZE, GFP_KERNEL)))
	{
        SCP_ERR("Allocate SCP_sensorFIFO fail\n");
		err = -ENOMEM;
		goto exit;
	}

    obj->SCP_sensorFIFO->wp = (struct SCP_sensorData*)0;//(struct SCP_sensorData *)((char *)obj->SCP_sensorFIFO + offsetof(struct sensorFIFO, data));
    obj->SCP_sensorFIFO->rp = (struct SCP_sensorData*)0;//(struct SCP_sensorData *)((char *)obj->SCP_sensorFIFO + offsetof(struct sensorFIFO, data));
    obj->SCP_sensorFIFO->FIFOSize = SCP_SENSOR_HUB_FIFO_SIZE - offsetof(struct sensorFIFO, data);
	obj->hw = get_cust_sensorHub_hw();

    SCP_ERR("obj->SCP_sensorFIFO = %p, wp = %p, rp = %p, size = %d\n", obj->SCP_sensorFIFO,
        obj->SCP_sensorFIFO->wp, obj->SCP_sensorFIFO->rp, obj->SCP_sensorFIFO->FIFOSize);

	obj_data = obj;
	
	atomic_set(&obj->trace, 0);
	atomic_set(&obj->suspend, 0);
    atomic_set(&obj->wait_rsp, 0);
    atomic_set(&obj->ipi_handler_running, 0);
    INIT_WORK(&obj->ipi_work, SCP_ipi_work);
    INIT_WORK(&obj->fifo_full_work, SCP_fifo_full_work);
#ifdef CUSTOM_KERNEL_STEP_COUNTER
    INIT_WORK(&obj->sd_work, SCP_sd_work);
    INIT_WORK(&obj->sm_work, SCP_sm_work);
#endif //#ifdef CUSTOM_KERNEL_STEP_COUNTER
    init_waitqueue_head(&SCP_sensorHub_req_wq);
    init_timer(&obj->timer);
    obj->timer.expires	= 3*HZ;
	obj->timer.function	= SCP_sensorHub_req_send_timeout;
	obj->timer.data		= (unsigned long)obj;
    md32_ipi_registration(IPI_SENSOR, SCP_sensorHub_IPI_handler, "SCP_sensorHub");
		
	if((err = misc_register(&SCP_sensorHub_device)))
	{
		SCP_ERR("SCP_sensorHub_device register failed\n");
		goto exit_misc_device_register_failed;
	}

	if((err = SCP_sensorHub_create_attr(&(SCP_sensorHub_init_info.platform_diver_addr->driver))))
	{
		SCP_ERR("create attribute err = %d\n", err);
		goto exit_create_attr_failed;
	}

    ctl.enable_hw_batch = SCP_sensorHub_enable_hw_batch;
	ctl.flush = SCP_sensorHub_flush;
	err = batch_register_control_path(MAX_ANDROID_SENSOR_NUM, &ctl);
	if(err)
	{
	 	SCP_ERR("register SCP sensor hub control path err\n");
		goto exit_kfree;
	}

	data.get_data = SCP_sensorHub_get_data;
    data.get_fifo_status = SCP_sensorHub_get_fifo_status;
	data.is_batch_supported = 1;
	err = batch_register_data_path(MAX_ANDROID_SENSOR_NUM, &data);
	if(err)
	{
	 	SCP_ERR("register SCP sensor hub control data path err\n");
		goto exit_kfree;
	}

#if defined(CONFIG_HAS_EARLYSUSPEND) && defined(USE_EARLY_SUSPEND)
	obj->early_drv.level    = EARLY_SUSPEND_LEVEL_DISABLE_FB - 1,
	obj->early_drv.suspend  = SCP_sensorHub_early_suspend,
	obj->early_drv.resume   = SCP_sensorHub_late_resume,
	register_early_suspend(&obj->early_drv);
#endif

	SCP_sensorHub_init_flag = 0;
	printk("%s: OK new\n", __func__);

	return 0;

	exit_create_attr_failed:
	misc_deregister(&SCP_sensorHub_device);
	exit_misc_device_register_failed:
	exit_kfree:
	kfree(obj);
	exit:
	SCP_ERR("%s: err = %d\n", __func__, err);
	SCP_sensorHub_init_flag = -1;
	return err;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_remove()
{
    struct sensorHub_hw *hw = get_cust_sensorHub_hw();
    int err = 0;

    SCP_FUN();
    SCP_sensorHub_power(hw, 0);

    if((err = SCP_sensorHub_delete_attr(&(SCP_sensorHub_init_info.platform_diver_addr->driver))))
	{
		SCP_ERR("SCP_sensorHub_delete_attr fail: %d\n", err);
	}
	
	if((err = misc_deregister(&SCP_sensorHub_device)))
	{
		SCP_ERR("misc_deregister fail: %d\n", err);
	}
    
    return 0;
}
/*----------------------------------------------------------------------------*/
#ifdef CUSTOM_KERNEL_STEP_COUNTER
static void SCP_sd_work(struct work_struct *work)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

	step_notify(TYPE_STEP_DETECTOR);
}
/*----------------------------------------------------------------------------*/
static void SCP_sm_work(struct work_struct *work)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

	step_notify(TYPE_SIGNIFICANT);
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_sd_handler(void* data, uint len)
{
    SCP_SENSOR_HUB_DATA_P rsp = (SCP_SENSOR_HUB_DATA_P)data;

    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    if (SCP_TRC_IPI & atomic_read(&(obj_data->trace)))
        SCP_LOG("len = %d, type = %d, action = %d, errCode = %d\n", len, rsp->rsp.sensorType, rsp->rsp.action, rsp->rsp.errCode);
    
	if(!obj_data)
	{
		return -1;
	}

    switch(rsp->rsp.action)
    {
        case SENSOR_HUB_NOTIFY:
            switch(rsp->notify_rsp.event)
            {
                case SCP_NOTIFY:
                    if (ID_STEP_DETECTOR == rsp->notify_rsp.sensorType)
                    {
                        schedule_work(&(obj_data->sd_work));
                    }
                    else
                    {
                        SCP_ERR("Unknow notify");
                    }
                    break;
                default:
                    SCP_ERR("Error sensor hub notify");
                    break;
            }
            break;
        default:
            SCP_ERR("Error sensor hub action");
            break;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_sm_handler(void* data, uint len)
{
    SCP_SENSOR_HUB_DATA_P rsp = (SCP_SENSOR_HUB_DATA_P)data;

    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    if (SCP_TRC_IPI & atomic_read(&(obj_data->trace)))
        SCP_LOG("len = %d, type = %d, action = %d, errCode = %d\n", len, rsp->rsp.sensorType, rsp->rsp.action, rsp->rsp.errCode);
    
	if(!obj_data)
	{
		return -1;
	}

    switch(rsp->rsp.action)
    {
        case SENSOR_HUB_NOTIFY:
            switch(rsp->notify_rsp.event)
            {
                case SCP_NOTIFY:
                    if (ID_SIGNIFICANT_MOTION == rsp->notify_rsp.sensorType)
                    {
                        schedule_work(&(obj_data->sm_work));
                    }
                    else
                    {
                        SCP_ERR("Unknow notify");
                    }
                    break;
                default:
                    SCP_ERR("Error sensor hub notify");
                    break;
            }
            break;
        default:
            SCP_ERR("Error sensor hub action");
            break;
    }

    return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensor_enable(int sensorType, int en)
{
    SCP_SENSOR_HUB_DATA req;
    int len;
    int err = 0;

    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    req.activate_req.sensorType = sensorType;
    req.activate_req.action = SENSOR_HUB_ACTIVATE;
    req.activate_req.enable = en;
    len = sizeof(req.activate_req);
    err = SCP_sensorHub_req_send(&req, &len, 1);
    if (err)
    {
        SCP_ERR("SCP_sensorHub_req_send fail!\n");
    }

    return err;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensor_get_data(int sensorType, void *value)
{
    SCP_SENSOR_HUB_DATA req;
    int len;
    int err = 0;

    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    req.get_data_req.sensorType = sensorType;
    req.get_data_req.action = SENSOR_HUB_GET_DATA;
    len = sizeof(req.get_data_req);
    err = SCP_sensorHub_req_send(&req, &len, 1);
    if (err)
    {
        SCP_ERR("SCP_sensorHub_req_send fail!\n");
    }

    switch (sensorType)
    {
        case ID_STEP_COUNTER:
            *((u64 *)value) = *((u64 *)req.get_data_rsp.int16_Data);
            break;
        case ID_STEP_DETECTOR:
            *((u32 *)value) = *((u16 *)req.get_data_rsp.int16_Data);
            break;
        case ID_SIGNIFICANT_MOTION:
            *((u32 *)value) = *((u16 *)req.get_data_rsp.int16_Data);
            break;
        default:
            err = -1;
            break;
    }

    SCP_LOG("sensorType = %d, value = %d\n", sensorType, *((u32 *)value));

    return err;
}
/*----------------------------------------------------------------------------*/
static int step_counter_open_report_data(int open)//open data rerport to HAL
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return 0;
}
/*----------------------------------------------------------------------------*/
static int step_counter_enable_nodata(int en)//only enable not report event to HAL
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return SCP_sensor_enable(ID_STEP_COUNTER, en);
}
/*----------------------------------------------------------------------------*/
static int step_detect_enable(int en)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return SCP_sensor_enable(ID_STEP_DETECTOR, en);
}
/*----------------------------------------------------------------------------*/
static int significant_motion_enable(int en)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return SCP_sensor_enable(ID_SIGNIFICANT_MOTION, en);
}
/*----------------------------------------------------------------------------*/
static int step_counter_set_delay(u64 delay)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return 0;
}
/*----------------------------------------------------------------------------*/
static int step_counter_get_data(u64 *value, int *status)
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    *status = 3;
    return SCP_sensor_get_data(ID_STEP_COUNTER, value);
}
/*----------------------------------------------------------------------------*/
static int step_detect_get_data(int *value )
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return SCP_sensor_get_data(ID_STEP_DETECTOR, value);
}
/*----------------------------------------------------------------------------*/
static int significant_motion_get_data(int *value )
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

    return SCP_sensor_get_data(ID_SIGNIFICANT_MOTION, value);
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_step_counter_init()
{
    struct step_c_control_path ctl={0};
	struct step_c_data_path data={0};
    
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();

	int err=0;
	//register step
	ctl.open_report_data= step_counter_open_report_data;
	ctl.enable_nodata = step_counter_enable_nodata;
	ctl.set_delay  = step_counter_set_delay;
	ctl.is_report_input_direct = false;
	ctl.is_support_batch = true;

	ctl.enable_significant = significant_motion_enable;
	ctl.enable_step_detect = step_detect_enable;
		
	err = step_c_register_control_path(&ctl);
	if(err)
	{
		printk("register step_counter control path err\n");
		return -1;
		
	}
	
	data.get_data = step_counter_get_data;
	data.get_data_significant = step_detect_get_data;
	data.get_data_step_d = significant_motion_get_data;
	data.vender_div = 1;
	err = step_c_register_data_path(&data);
	if(err)
	{
	 	printk("register step counter data path err\n");
		return -1;
	}

    SCP_sensorHub_rsp_registration(ID_SIGNIFICANT_MOTION, SCP_sensorHub_sm_handler);
    SCP_sensorHub_rsp_registration(ID_STEP_DETECTOR, SCP_sensorHub_sd_handler);

    return 0;
}
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_step_counter_uninit()
{
    if (SCP_TRC_FUN & atomic_read(&(obj_data->trace)))
        SCP_FUN();
    
    return 0;
}
#endif //#ifdef CUSTOM_KERNEL_STEP_COUNTER
/*----------------------------------------------------------------------------*/
static int SCP_sensorHub_local_init(void)
{
    SCP_sensorHub_probe();

	if(-1 == SCP_sensorHub_init_flag)
	{
	   return -1;
	}
	return 0;
}
/*----------------------------------------------------------------------------*/
static int __init SCP_sensorHub_init(void)
{
	SCP_FUN();
	batch_driver_add(&SCP_sensorHub_init_info);
#ifdef CUSTOM_KERNEL_STEP_COUNTER
    step_c_driver_add(&SCP_pedometer_init_info);
#endif //#ifdef CUSTOM_KERNEL_STEP_COUNTER

	return 0;    
}
/*----------------------------------------------------------------------------*/
static void __exit SCP_sensorHub_exit(void)
{
	SCP_FUN();
}
/*----------------------------------------------------------------------------*/
//late_initcall(SCP_sensorHub_init);
module_init(SCP_sensorHub_init);
module_exit(SCP_sensorHub_exit);
/*----------------------------------------------------------------------------*/
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SCP sensor hub driver");
MODULE_AUTHOR("andrew.yang@mediatek.com");
