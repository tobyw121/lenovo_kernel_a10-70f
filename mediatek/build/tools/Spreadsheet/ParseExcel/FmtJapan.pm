# DISCLAIMER OF WARRANTY
# Because this software is licensed free of charge, there is no warranty for the software,
# to the extent permitted by applicable law. Except when otherwise stated in writing
# the copyright holders and/or other parties provide the software "as is" without
# warranty of any kind, either expressed or implied, including, but not limited to,
# the implied warranties of merchantability and fitness for a particular purpose.
# The entire risk as to the quality and performance of the software is with you.
# Should the software prove defective, you assume the cost of all necessary
# servicing, repair, or correction.

# In no event unless required by applicable law or agreed to in writing will any
# copyright holder, or any other party who may modify and/or redistribute the software
# as permitted by the above licence, be liable to you for damages, including any general,
# special, incidental, or consequential damages arising out of the use or inability
# to use the software (including but not limited to loss of data or data being rendered
# inaccurate or losses sustained by you or third parties or a failure of the software
# to operate with any other software), even if such holder or other party
# has been advised of the possibility of such damages.

# AUTHOR
# Current maintainer 0.40+: John McNamara jmcnamara@cpan.org
# Maintainer 0.27-0.33: Gabor Szabo szabgab@cpan.org
# Original author: Kawai Takanori (Hippo2000) kwitknr@cpan.org

# COPYRIGHT
# Copyright (c) 2009-2010 John McNamara
# Copyright (c) 2006-2008 Gabor Szabo
# Copyright (c) 2000-2006 Kawai Takanori
# All rights reserved. This is free software. You may distribute under the terms of
# the Artistic License(full text of the Artistic License http://dev.perl.org/licenses/artistic.html).

package Spreadsheet::ParseExcel::FmtJapan;
use utf8;

###############################################################################
#
# Spreadsheet::ParseExcel::FmtJapan - A class for Cell formats.
#
# Used in conjunction with Spreadsheet::ParseExcel.
#
# Copyright (c) 2009      John McNamara
# Copyright (c) 2006-2008 Gabor Szabo
# Copyright (c) 2000-2006 Kawai Takanori
#
# perltidy with standard settings.
#
# Documentation after __END__
#

use strict;
use warnings;

use Encode qw(find_encoding decode);
use base 'Spreadsheet::ParseExcel::FmtDefault';
our $VERSION = '0.57';

my %FormatTable = (
    0x00 => '@',
    0x01 => '0',
    0x02 => '0.00',
    0x03 => '#,##0',
    0x04 => '#,##0.00',
    0x05 => '(\\#,##0_);(\\#,##0)',
    0x06 => '(\\#,##0_);[RED](\\#,##0)',
    0x07 => '(\\#,##0.00_);(\\#,##0.00_)',
    0x08 => '(\\#,##0.00_);[RED](\\#,##0.00_)',
    0x09 => '0%',
    0x0A => '0.00%',
    0x0B => '0.00E+00',
    0x0C => '# ?/?',
    0x0D => '# ??/??',

    #    0x0E => 'm/d/yy',
    0x0E => 'yyyy/m/d',
    0x0F => 'd-mmm-yy',
    0x10 => 'd-mmm',
    0x11 => 'mmm-yy',
    0x12 => 'h:mm AM/PM',
    0x13 => 'h:mm:ss AM/PM',
    0x14 => 'h:mm',
    0x15 => 'h:mm:ss',

    #    0x16 => 'm/d/yy h:mm',
    0x16 => 'yyyy/m/d h:mm',

    #0x17-0x24 -- Differs in Natinal
    0x1E => 'm/d/yy',
    0x1F => 'yyyy"???"m"???"d"???"',
    0x20 => 'h"???"mm"???"',
    0x21 => 'h"???"mm"???"ss"???"',

    #0x17-0x24 -- Differs in Natinal
    0x25 => '(#,##0_);(#,##0)',
    0x26 => '(#,##0_);[RED](#,##0)',
    0x27 => '(#,##0.00);(#,##0.00)',
    0x28 => '(#,##0.00);[RED](#,##0.00)',
    0x29 => '_(*#,##0_);_(*(#,##0);_(*"-"_);_(@_)',
    0x2A => '_(\\*#,##0_);_(\\*(#,##0);_(*"-"_);_(@_)',
    0x2B => '_(*#,##0.00_);_(*(#,##0.00);_(*"-"??_);_(@_)',
    0x2C => '_(\\*#,##0.00_);_(\\*(#,##0.00);_(*"-"??_);_(@_)',
    0x2D => 'mm:ss',
    0x2E => '[h]:mm:ss',
    0x2F => 'mm:ss.0',
    0x30 => '##0.0E+0',
    0x31 => '@',

    0x37 => 'yyyy"???"m"???"',
    0x38 => 'm"???"d"???"',
    0x39 => 'ge.m.d',
    0x3A => 'ggge"???"m"???"d"???"',
);

#------------------------------------------------------------------------------
# new (for Spreadsheet::ParseExcel::FmtJapan)
#------------------------------------------------------------------------------
sub new {
    my ( $class, %args ) = @_;
    my $encoding = $args{Code} || $args{encoding};
    my $self = { Code => $encoding };
    if($encoding){
        $self->{encoding} = find_encoding($encoding eq 'sjis' ? 'cp932' : $encoding)
            or do{
                require Carp;
                Carp::croak(qq{Unknown encoding '$encoding'});
            };
    }
    return bless $self, $class;
}

#------------------------------------------------------------------------------
# TextFmt (for Spreadsheet::ParseExcel::FmtJapan)
#------------------------------------------------------------------------------
sub TextFmt {
    my ( $self, $text, $input_encoding ) = @_;
    if(!defined $input_encoding){
        $input_encoding = 'utf8';
    }
    elsif($input_encoding eq '_native_'){
        $input_encoding = 'cp932'; # Shift_JIS in Microsoft products
    }
    $text = decode($input_encoding, $text);
    return $self->{Code} ? $self->{encoding}->encode($text) : $text;
}
#------------------------------------------------------------------------------
# FmtStringDef (for Spreadsheet::ParseExcel::FmtJapan)
#------------------------------------------------------------------------------
sub FmtStringDef {
    my ( $self, $format_index, $book ) = @_;
    return $self->SUPER::FmtStringDef( $format_index, $book, \%FormatTable );
}

#------------------------------------------------------------------------------
# CnvNengo (for Spreadsheet::ParseExcel::FmtJapan)
#------------------------------------------------------------------------------

# Convert A.D. into Japanese Nengo (aka Gengo)

my @Nengo = (
	{
		name      => '??????', # Heisei
		abbr_name => 'H',

		base      => 1988,
		start     => 19890108,
	},
	{
		name      => '??????', # Showa
		abbr_name => 'S',

		base      => 1925,
		start     => 19261225,
	},
	{
		name      => '??????', # Taisho
		abbr_name => 'T',

		base      => 1911,
		start     => 19120730,
	},
	{
		name      => '??????', # Meiji
		abbr_name => 'M',

		base      => 1867,
		start     => 18680908,
	},
);

# Usage: CnvNengo(name => @tm) or CnvNeng(abbr_name => @tm)
sub CnvNengo {
    my ( $kind, @tm ) = @_;
    my $year = $tm[5] + 1900;
    my $wk = ($year * 10000) + ($tm[4] * 100) + ($tm[3] * 1);
    #my $wk = sprintf( '%04d%02d%02d', $year, $tm[4], $tm[3] );
    foreach my $nengo(@Nengo){
        if( $wk >= $nengo->{start} ){
            return $nengo->{$kind} . ($year - $nengo->{base});
        }
    }
    return $year;
}

1;

__END__

=pod

=head1 NAME

Spreadsheet::ParseExcel::FmtJapan - A class for Cell formats.

=head1 SYNOPSIS

See the documentation for Spreadsheet::ParseExcel.

=head1 DESCRIPTION

This module is used in conjunction with Spreadsheet::ParseExcel. See the documentation for Spreadsheet::ParseExcel.

=head1 AUTHOR

Maintainer 0.40+: John McNamara jmcnamara@cpan.org

Maintainer 0.27-0.33: Gabor Szabo szabgab@cpan.org

Original author: Kawai Takanori kwitknr@cpan.org

=head1 COPYRIGHT

Copyright (c) 2009-2010 John McNamara

Copyright (c) 2006-2008 Gabor Szabo

Copyright (c) 2000-2006 Kawai Takanori

All rights reserved.

You may distribute under the terms of either the GNU General Public License or the Artistic License, as specified in the Perl README file.

=cut
