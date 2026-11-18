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

# Deploy (DOMINO_DIR comes from the Makefile, or from the environment if set)
DOMINO_DIR=$(make -s print-domino-dir)
if [ -z "$DOMINO_DIR" ] || [ ! -d "$DOMINO_DIR" ]; then
	echo "Domino directory not found: '$DOMINO_DIR'"
	exit 1
fi
rm -f "$DOMINO_DIR/libsmtp-tarpit.so"
mv libsmtp-tarpit.so "$DOMINO_DIR/"
echo "Deploy complete ($DOMINO_DIR)."