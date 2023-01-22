#!/usr/bin/perl
#
# decode-alios-ota-firmware.pl -- Decode AliOs OTA firmware (application) file
#
# (C) 2023 Hajo Noerenberg
#
# Usage: decode-alios-otafirmware.pl solis-s3-app-1012F_ota.bin
#
# http://www.noerenberg.de/
# https://github.com/hn/ginlong-solis
#
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 3.0 as
# published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program. If not, see <http://www.gnu.org/licenses/gpl-3.0.txt>.
#

use strict;
use Digest::MD5 qw(md5);

my $signature = "81958711";
my $magic     = "\xef\xef\xef\xef";	# OTA_BIN_TYPE_MAGIC_SINGLE

my $f = $ARGV[0];
open( IF, "<$f" ) || die( "Unable to open input file '$f': " . $! );
binmode(IF);

my $buf;
my $md5buf;
my $fsize = -s $f;

sub readbytes {
    my ( $len, $desc ) = @_;
    printf( "  0x%08x:", tell(IF) );
    read( IF, $buf, $len ) == $len || die;
    $md5buf .= $buf;
    printf( "  %-16s = ", $desc );
}

for ( my $i = 1 ; $i <= 2 ; $i++ ) {
    printf( "# Segment %s\n", ( $i == 1 ) ? ".text" : ".data" );
    readbytes( 8, "Signature" );
    printf( "0x%s ", unpack( "H*", $buf ) );
    if ( $buf eq $signature ) {
        print "(OK)\n";
    } else {
        printf( "(INVALID, 0x%s)\n", unpack( "H*", $signature ) );
        exit;
    }
    readbytes( 4, "Code length" );
    my $len = unpack( "V", $buf );
    printf( "%d\n", $len );
    readbytes( 4, "Address" );
    my $addr = unpack( "V", $buf );
    printf( "0x%08x ", $addr );

    if ( $addr >= hex("0x10000000") ) {
        print "(RAM)\n";
    } else {
        print "(FLASH XIP)\n";
    }
    readbytes( 16, "Reserved" );
    printf( "0x%s\n", unpack( "H*", $buf ) );

    readbytes( $len, "Code" );
    print "(byte code)\n\n";
}

print "# Segments checksum\n";	# AliOS-Things/platform/mcu/rtl8710bn/tools/checksum
my $fletcher = 0;
for ( my $i = 0 ; $i < length($md5buf) ; $i++ ) {
    $fletcher = ( $fletcher + ord( substr( $md5buf, $i, 1 ) ) ) % 4294967296;
}
readbytes( 4, "Checksum" );
printf( "0x%s ", unpack( "H*", $buf ) );
if ( unpack( "V", $buf ) eq $fletcher ) {
    print "(OK)\n";
} else {
    printf( "(INVALID, 0x%x)\n", unpack( "V", pack( "N", $fletcher ) ) );
    exit;
}

print "\n# OTA trailer\n";	# AliOS-Things/build/scripts/ota_gen_md5_bin.py , middleware/uagent/ota/ota_core/ble/ota_breeze.h
my $md5sum = md5($md5buf);
my $loc    = tell(IF);
readbytes( 4, "Magic" );
printf( "0x%s ", unpack( "H*", $buf ) );
if ( $buf eq $magic ) {
    print "(OK)\n";
} else {
    printf( "(INVALID, 0x%x)\n", unpack( "H*", $magic ) );
    exit;
}
readbytes( 4, "Size" );
my $osize = unpack( "V", $buf );
printf( "%d ", $osize );
if ( $osize == $loc ) {
    print "(OK)\n";
} else {
    printf( "(INVALID, %d)\n", $loc );
    exit;
}
readbytes( 16, "MD5 checksum" );
printf( "0x%s ", unpack( "H*", $buf ) );
if ( $buf eq $md5sum ) {
    print "(OK)\n";
} else {
    printf( "(INVALID, 0x%s)\n", unpack( "H*", $md5sum ) );
    exit;
}
readbytes( 4, "Reserved" );
printf( "0x%s\n", unpack( "H*", $buf ) );

print "\n# End of data\n";
$loc = tell(IF);
printf( "  0x%08x:  %-16s = 0x%x = %d ", $loc, "Filesize", $fsize, $fsize );
if ( $loc == $fsize ) {
    print "(OK)\n";
} else {
    printf( "(INVALID, 0x%x)\n", $loc );
    exit;
}

