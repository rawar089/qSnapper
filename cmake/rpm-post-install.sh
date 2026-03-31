#!/bin/bash
# RPM %post scriptlet: Load SELinux policy module and relabel files

POLICY_PP="/usr/share/selinux/packages/qsnapper.pp"

if [ -x /usr/sbin/semodule ] && [ -f "$POLICY_PP" ]; then
    /usr/sbin/semodule -i "$POLICY_PP" >/dev/null 2>&1 || :
fi

if [ -x /usr/sbin/restorecon ]; then
    /usr/sbin/restorecon -R /usr/bin/qsnapper >/dev/null 2>&1 || :
    /usr/sbin/restorecon -R /usr/libexec/qsnapper-dbus-service >/dev/null 2>&1 || :
fi
