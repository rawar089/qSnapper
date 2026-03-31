#!/bin/bash
# RPM %preun scriptlet: Remove SELinux policy module on uninstall (not upgrade)

# $1 == 0 means full removal, $1 == 1 means upgrade
if [ "$1" -eq 0 ]; then
    if [ -x /usr/sbin/semodule ]; then
        /usr/sbin/semodule -r qsnapper >/dev/null 2>&1 || :
    fi
fi
