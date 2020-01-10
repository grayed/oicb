#!/bin/sh

for t in $(ls tests/test-* | sort -R); do
	sh ./"$t"
done
