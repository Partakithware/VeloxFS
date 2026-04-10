SIGN_TOOL="/usr/src/linux-headers-$(uname -r)/scripts/sign-file"

sudo $SIGN_TOOL sha512 ~/Downloads/MOK-CERT/MOK.priv ~/Downloads/MOK-CERT/MOK.der veloxfs_mod.ko

read p