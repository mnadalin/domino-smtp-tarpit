#!/bin/bash

# Remove old build
if [ -f libsmtp-tarpit.so ]; then
	rm -f libsmtp-tarpit.so
fi

# Build
make
if [ ! -f libsmtp-tarpit.so ]; then
	echo "Build failed!"
	exit 1
fi

# Deploy
rm -f /opt/hcl/domino/notes/14050000/linux/libsmtp-tarpit.so
mv libsmtp-tarpit.so /opt/hcl/domino/notes/14050000/linux/
echo "Deploy complete."