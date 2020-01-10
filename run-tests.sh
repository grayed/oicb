#!/bin/sh

for t in tests/test-*; do
	sh ./"$t"
done
