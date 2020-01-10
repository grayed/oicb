OICB_DIR="${OICB_DIR:-$PWD/obj}"
ICBD_PORT=${ICBD_PORT:-8398}
SUDO=${SUDO:-/usr/bin/doas}

ICBD_PID=
ICB_PIDS=
TEST_LOG="$OICB_DIR/${0##*/}.log"
TEST_NAME=${0##*/test-}
FAIL_CNT=0
ICB_RUN_NUM=

icbd=$(command -v icbd)
if [ $? -ne 0 ]; then
	echo "${0##*/}: please install icbd first" >&2
	exit 1
fi

HOME="$OICB_DIR"
rm -Rf "${OICB_DIR}/.oicb"

# 1. Inserts @host:port into penultimate argument
# 2. Runs expect(1) that in turn runs oicb and read script from stdin.
run_oicb() {
	local args idx expect_log fail_text

	idx=$(($# - 2))
	set -A args -- "$@"
	login=${args[$idx]}
	args[$idx]="${login}@127.0.0.1:$ICBD_PORT"

	expect_log="${TEST_LOG}.expect${ICB_RUN_NUM:+-$ICB_RUN_NUM}"
	fail_text="FAIL${ICB_RUN_NUM:+ $ICB_RUN_NUM}"

	{ {
		echo "set timeout 3"
		echo "log_file -a -noappend \"${expect_log}\""
		echo "log_user 0"
		echo 'set oicb_args [lrange $argv 0 end]'
		echo "spawn -noecho \"${OICB_DIR}/oicb\" {*}\$oicb_args"
		cat
	} | expect -b - -- "${args[@]}"; } || {
		FAIL_CNT=$(($FAIL_CNT + 1))
		echo "$fail_text"
		echo "expect log:"
		cat "$expect_log"
		echo
		return 1
	}
}

run_icbd() {
	local icbd_logdir="$OICB_DIR/icbdlogs-$TEST_NAME"

	# do NOT use 'mkdir -p', we don't want to create OICB_DIR accidentally
	test -d "$icbd_logdir" || mkdir "$icbd_logdir"
	$SUDO "$icbd" -d -S "oicb test server" -G roomfoo,roombar \
	        -L "$icbd_logdir" \
	        -4 "127.0.0.1:${ICBD_PORT}" \
		>"$icbd_logdir/icbd.log" 2>&1 &
	ICBD_PID=$!
}

kill_icbd() {
	$SUDO kill $ICBD_PID
	wait $ICBD_PID >/dev/null 2>&1
	ICBD_PID=
}

finish() {
	test -z "$ICBD_PID" || kill_icbd || true
	test $FAIL_CNT -lt 0 || echo OK
}

trap finish EXIT

echo "===> oicb test $TEST_NAME"
