#!/usr/bin/perl
#
# ginlong2influx.pl
#
# Read basic stats from Ginlong Solis WiFi Stick S3 and feed them to influx db
#
# (C) 2022 Hajo Noerenberg
#
# http://www.noerenberg.de/
# https://github.com/hn/ginlong-solis
#
# apt-get install libwww-perl libinfluxdb-lineprotocol-perl
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
use InfluxDB::LineProtocol qw(data2line);
use LWP::UserAgent;
use HTTP::Request::Common;

my $debug = -t STDIN;

my $ginlonghost = '192.168.0.123:80';		# WiFi stick IP address
my $ginlonguser = 'admin';
#my $ginlongpass	= '123456789';		# password BEFORE joining your home WiFi
my $ginlongpass = 'yOUrHoMeWiFipassWoRd';	# password AFTER joining your network (password = your WiFi password)

my $influxhost = '192.168.0.234:8086';		# Influx DB IP address
my $influxdb   = 'smarthome';
my $influxmeas = 'ginlong';
my $influxuser;
my $influxpass;

sub floatify {
    return $_[0] . ".0" unless ( $_[0] =~ /\./ );
    return $_[0];
}

my $ginlongstats = 'http://' . $ginlonghost . '/inverter.cgi';
my $influx       = 'http://' . $influxhost . '/write?precision=ns&db=' . $influxdb;
my $influxreq;
my $ua = LWP::UserAgent->new();

my $req = HTTP::Request->new( 'GET' => $ginlongstats );
$req->authorization_basic( $ginlonguser, $ginlongpass ) if ( $ginlonguser && $ginlongpass );
my $res = $ua->request($req);

exit 1 if ( $res->code == 500 );	# fail quietly for "500 Unable to connect", inverter may sleep

unless ( $res->is_success ) {
    print STDERR "Unable to read inverter status: " . $res->status_line . "\n\n" . $res->headers()->as_string;
    exit 2;
}

my $idata = $res->content;
$idata =~ s/[^[:print:]]+//g;		# remove spurious non-printable characters
print $idata . "\n" if ($debug);	# 1801230123456789;780034;102;28.7;40;0;0;NO;

my ( $iserial, $ifirmware, $imodel, $itemp, $icurrpower, $iyieldtoday, $iyieldtotal, $ialerts ) = split( /;/, $idata );

exit 3 if ( $iserial =~ /^0+$/ );	# during startup the inverter sends invalid data

$influxreq .= data2line( $influxmeas, { 'curr_power'  => floatify($icurrpower),  'serial' => $iserial } ) . "\n";
$influxreq .= data2line( $influxmeas, { 'yield_today' => floatify($iyieldtoday), 'serial' => $iserial } ) . "\n";
$influxreq .= data2line( $influxmeas, { 'yield_total' => floatify($iyieldtotal), 'serial' => $iserial } ) . "\n";
$influxreq .= data2line( $influxmeas, { 'temperature' => floatify($itemp),       'serial' => $iserial } ) . "\n";

print $influxreq . "\n" if ($debug);

my $ireq = HTTP::Request->new( 'POST' => $influx, [ 'Content_Type' => 'application/x-www-form-urlencoded' ], $influxreq );
$ireq->authorization_basic( $influxuser, $influxpass ) if ( $influxuser && $influxpass );
my $ires = $ua->request($ireq);

print $ires->status_line . "\n" . $ires->headers()->as_string if ($debug);    # HTTP 204 is ok

