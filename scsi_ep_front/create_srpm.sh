#!/bin/sh

if [ "$#" -ne "1" ]; then
echo "Usage $0 <rpm_name>"
exit 0
fi

DRIVER_BASE=$(cat buildversion | cut -d '.' -f1,2)
DRIVER_RELEASE=$(cat buildversion | cut -d '.' -f3)

RMP_PKG_NAME=$1-$DRIVER_BASE

echo "%define name $1" > driver_rpm.spec
echo "%define version $DRIVER_BASE" >> driver_rpm.spec
echo "%define release $DRIVER_RELEASE" >> driver_rpm.spec
cat rpm_spec_template >> driver_rpm.spec

mkdir $RMP_PKG_NAME
cp -r *.c *.h buildversion Makefile driver_rpm.spec  $RMP_PKG_NAME
tar czf $RMP_PKG_NAME.tgz $RMP_PKG_NAME
rpmbuild -ts --define "_srcrpmdir ." --define "_specdir /tmp" $RMP_PKG_NAME.tgz 
rm -rf $RMP_PKG_NAME
rm $RMP_PKG_NAME.tgz
rm driver_rpm.spec

#simple step to build binary rpm
rpmbuild --rebuild --define "_srcrpmdir ." --define "_rpmdir ." --define "_specdir /tmp" --define "_topdir /tmp" --define "_builddir /tmp" $RMP_PKG_NAME-$DRIVER_RELEASE.src.rpm
