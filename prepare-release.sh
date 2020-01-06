#!/bin/sh

set -e

if [ $# -eq 0 ]; then
	echo "usage: ${0##*/} tag [savepath]" >&2
	exit 1
fi

tag=$1; shift
if ! echo "$tag" | egrep -q '^v[0-9]+\.[0-9]+\.[0-9]+$'; then
	echo "invalid tag name, must be like v1.2.3" >&2
	exit 1
fi

arc_path=/tmp
if [ $# -gt 0 ]; then
	arc_path=$1; shift
fi

v=${tag#v}
arc_name=oicb-${v}.tar.gz

if git tag | fgrep -qx "$tag"; then
	echo "tag $tag is already there, skipping it's creation"
else
	git tag "$tag"
	git push origin "$tag"
fi

pax -wzv \
	-s ',^\./.git\(/.*\)*$,,' \
	-s ',^\./obj\(/.*\)*$,,' \
	-s ',^\./build.*,,' \
	-s "/^./oicb-${v}/" \
	. >"$arc_path/$arc_name"
echo "archive saved to $arc_path/$arc_name"
