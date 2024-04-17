#!/bin/bash

# As of Linux Kernel 5.8, the minimum gcc needed to build is 4.9
# The stock 4.8 gcc on CentOS is unable to build.
# elrepo appears to have standardized on using devtoolset-9 for 5.8+ kernels
# to address this issue.  We follow their lead.

# use /etc/os-release which systemd has pushed as a standard to determine a
# CentOS host since the CentOS minimal installer does not install lsb_release by
# default
OS=$(cat /etc/os-release | grep "ID" | head -n1 | awk -F"=" '{ print $2 }' | tr -d '"')
if [ ${OS} == "centos" ]
then
	let major=`echo $1 | cut -d "." -f 1`
	let minor=`echo $1 | cut -d "." -f 2`

	if [ $major -gt 5 -o $major -eq 5 -a $minor -gt 7 ]
	then
		/bin/scl enable devtoolset-9 -- $2
	else
		$2
	fi
else
	$2
fi
