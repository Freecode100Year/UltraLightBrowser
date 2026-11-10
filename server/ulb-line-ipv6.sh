#!/usr/bin/env bash
# UltraLightBrowser 线路服务器：每 3 天自动更换一个入口 IPv6 地址
# Line server: answer on a new IPv6 address every 3 days.
#
# 浏览器和服务器用同一个规则算地址（不需要 DNS、不用通知浏览器）：
#   周期 = Unix 秒数 / 259200（3 天）
#   后缀 = SHA-256("ulb-line-v1|" + 线路UUID + "|" + 周期) 的前 8 字节
#   地址 = 你的 /64 前缀 + 后缀
# 与 warp/rotate.go 完全一致。浏览器依次尝试本周期、上一周期的地址，最后才用链接里的固定地址。
#
# 用法 Usage（需要 root）：
#   sudo bash ulb-line-ipv6.sh install --uuid <线路UUID> --prefix 2001:db8:1:2::/64 [--dev he-ipv6] [--port 443] [--dnat-to <固定地址>]
#   sudo bash ulb-line-ipv6.sh status
#   sudo bash ulb-line-ipv6.sh apply        （定时器每小时自动运行这一步）
#   sudo bash ulb-line-ipv6.sh uninstall
#   bash ulb-line-ipv6.sh show --uuid <UUID> --prefix <前缀/64> [--period N]   只计算地址，不改系统
#
# 要求：服务器有一个路由到本机的 IPv6 /64（例如 Hurricane Electric 隧道的 Routed /64）。
# 两种方式让 Xray 收到发往新地址的连接：
#   1. Xray 入站监听 [::]（所有地址）：脚本把三个周期的地址加到网卡上即可。
#   2. --dnat-to <线路原来的固定地址>：Xray 只监听那个固定地址（例如同一端口上还有别的线路）时用。
#      脚本用独立的 nftables 表 ulb_line，把发往三个周期地址的连接转到固定地址，不改 Xray、不重启 Xray，
#      也不动其他防火墙规则。整段 /64 已经路由到本机（ip -6 route show table local 里有 local <前缀>）时不再加地址。
set -euo pipefail

CONF=/etc/ulb-line-ipv6.conf
STATE=/var/lib/ulb-line-ipv6/managed
UNIT=ulb-line-ipv6
SELF=/usr/local/sbin/ulb-line-ipv6
PERIOD_SECONDS=259200

die() { echo "错误 Error: $*" >&2; exit 1; }
info() { echo "==> $*"; }

UUID="" PREFIX="" DEV="" PORT=443 PERIOD="" DNAT_TO=""
parse_args() {
	while [ $# -gt 0 ]; do
		case "$1" in
		--uuid) UUID=${2,,}; shift 2 ;;
		--prefix) PREFIX=$2; shift 2 ;;
		--dev) DEV=$2; shift 2 ;;
		--port) PORT=$2; shift 2 ;;
		--period) PERIOD=$2; shift 2 ;;
		--dnat-to) DNAT_TO=$2; shift 2 ;;
		*) die "未知参数 unknown option: $1" ;;
		esac
	done
}

# 前缀的前 4 组（16 位一组），要求写成 a:b:c:d::/64
prefix_groups() {
	local p=${PREFIX%/64}
	[ "$p" != "$PREFIX" ] || die "--prefix 必须是 /64，例如 2001:db8:1:2::/64"
	p=${p%::}
	IFS=: read -r g1 g2 g3 g4 extra <<<"$p"
	[ -n "${g4:-}" ] && [ -z "${extra:-}" ] || die "--prefix 请写成 4 组加 ::/64 的形式，例如 2001:db8:1:2::/64"
	printf '%x:%x:%x:%x' "0x$g1" "0x$g2" "0x$g3" "0x$g4"
}

# 某个周期的地址
addr_for() {
	local period=$1 h
	h=$(printf 'ulb-line-v1|%s|%d' "$UUID" "$period" | sha256sum | cut -c1-16)
	[ "$h" != 0000000000000000 ] || h=0000000000000001
	printf '%s:%x:%x:%x:%x\n' "$(prefix_groups)" "0x${h:0:4}" "0x${h:4:4}" "0x${h:8:4}" "0x${h:12:4}"
}

now_period() { echo $(($(date +%s) / PERIOD_SECONDS)); }

load_conf() {
	[ -r "$CONF" ] || die "还没有安装，请先运行 install"
	# shellcheck disable=SC1090
	. "$CONF"
}

# 找到持有该 /64 里某个地址的网卡
detect_dev() {
	local first
	first=$(prefix_groups)
	ip -6 -o addr show scope global | awk -v p="$first:" 'index($4, p) == 1 {print $2; exit}'
}

# 整段前缀是否已经路由到本机（AnyIP）
prefix_is_local() {
	local first
	first=$(prefix_groups)
	ip -6 route show table local | grep -qiE "^local ${first}::/64 "
}

# 写入（整表替换）转发规则；nft -f 是原子的，出错时原有规则不变
apply_dnat() {
	local set
	set=$(echo "$1" | paste -sd, -)
	nft -f - <<NFT
table ip6 ulb_line
delete table ip6 ulb_line
table ip6 ulb_line {
	chain prerouting {
		type nat hook prerouting priority dstnat; policy accept;
		ip6 daddr { $set } tcp dport $PORT dnat to [$DNAT_TO]:$PORT
	}
	chain output {
		type nat hook output priority -100; policy accept;
		ip6 daddr { $set } tcp dport $PORT dnat to [$DNAT_TO]:$PORT
	}
}
NFT
}

check_listen() {
	local out
	if [ -n "${DNAT_TO:-}" ]; then
		if ss -ltnH "sport = :$PORT" 2>/dev/null | awk '{print $4}' | grep -qiF "[$DNAT_TO]:$PORT"; then
			echo "  ✓ 固定地址 [$DNAT_TO]:$PORT 有程序在监听，转发目标可用"
		else
			echo "  ! [$DNAT_TO]:$PORT 没有程序在监听，转发过去也连不上（Xray 没运行？地址写错？）"
		fi
		nft list table ip6 ulb_line >/dev/null 2>&1 && echo "  ✓ 转发规则已生效（nft list table ip6 ulb_line 查看）" || echo "  ! 转发规则不存在"
		return
	fi
	out=$(ss -ltnH "sport = :$PORT" 2>/dev/null || true)
	if [ -z "$out" ]; then
		echo "  ! 没有程序在监听 TCP $PORT（Xray 没运行？）"
	elif echo "$out" | awk '{print $4}' | grep -qE '^(\[::\]|\*|::):'"$PORT"'$'; then
		echo "  ✓ 端口 $PORT 监听在所有地址上，新地址可以直接使用"
	else
		echo "  ! 端口 $PORT 只监听在固定地址上：$(echo "$out" | awk '{print $4}' | xargs)"
		echo "    请把 Xray 入站的 \"listen\" 改成 \"::\"（或删掉 listen），然后重启 Xray，否则新地址连不上"
	fi
}

apply() {
	load_conf
	local n want cur a
	n=$(now_period)
	# 上一周期、本周期、下一周期：容许双方时钟有偏差，也让换周期的那一刻不断线
	want=$(for p in $((n - 1)) "$n" $((n + 1)); do addr_for "$p"; done)
	mkdir -p "$(dirname "$STATE")"
	touch "$STATE"
	if [ -n "${DNAT_TO:-}" ] && prefix_is_local; then
		: # 整段 /64 已归本机，不用加地址
	else
		for a in $want; do
			# replace：已有就保持，没有就添加（重复运行不会报错）
			ip -6 addr replace "$a/128" dev "$DEV" nodad
		done
	fi
	[ -z "${DNAT_TO:-}" ] || apply_dnat "$want"
	# 删除以前由本脚本添加、现在已过期的地址（不碰其他地址）
	while read -r cur; do
		[ -n "$cur" ] || continue
		if ! grep -qxF "$cur" <<<"$want"; then
			ip -6 addr del "$cur/128" dev "$DEV" 2>/dev/null && info "已删除 removed $cur" || true
		fi
	done <"$STATE"
	echo "$want" >"$STATE"
	echo "本周期地址 current: [$(addr_for "$n")]:$PORT"
}

status() {
	load_conf
	local n
	n=$(now_period)
	echo "网卡 dev: $DEV   前缀 prefix: $PREFIX   端口 port: $PORT${DNAT_TO:+   转发到 dnat-to: [$DNAT_TO]:$PORT}"
	echo "本周期 current ($n):  $(addr_for "$n")"
	echo "下周期 next ($((n + 1))): $(addr_for $((n + 1)))  （$(date -d @$(((n + 1) * PERIOD_SECONDS)) '+%F %T %Z') 起）"
	echo "由本脚本管理的地址 managed:"
	sed 's/^/  /' "$STATE" 2>/dev/null || echo "  （无）"
	check_listen
	systemctl --no-pager --quiet is-active "$UNIT.timer" && echo "  ✓ 定时器运行中 timer active" || echo "  ! 定时器未运行 timer inactive"
}

install_all() {
	[ "$(id -u)" = 0 ] || die "请用 root 运行（sudo）"
	[ -n "$UUID" ] || die "缺少 --uuid（线路链接 vless:// 后面、@ 前面那一串）"
	[ -n "$PREFIX" ] || die "缺少 --prefix（你的 IPv6 /64，例如 2001:db8:1:2::/64）"
	prefix_groups >/dev/null
	[ -n "$DEV" ] || DEV=$(detect_dev)
	[ -n "$DEV" ] || die "找不到持有该前缀地址的网卡，请用 --dev 指定（HE 隧道一般是 he-ipv6）"
	ip link show "$DEV" >/dev/null 2>&1 || die "网卡 $DEV 不存在"
	if [ -n "$DNAT_TO" ]; then
		command -v nft >/dev/null || die "--dnat-to 需要 nftables（apt install nftables）"
		[[ "$DNAT_TO" == *:* ]] || die "--dnat-to 要写 IPv6 地址"
	fi
	install -m 755 "$0" "$SELF"
	umask 077
	printf 'UUID=%q\nPREFIX=%q\nDEV=%q\nPORT=%q\nDNAT_TO=%q\n' "$UUID" "$PREFIX" "$DEV" "$PORT" "$DNAT_TO" >"$CONF"
	cat >/etc/systemd/system/$UNIT.service <<EOF
[Unit]
Description=UltraLightBrowser line: rotate the IPv6 entry address every 3 days
After=network-online.target
Wants=network-online.target

[Service]
Type=oneshot
ExecStart=$SELF apply
EOF
	cat >/etc/systemd/system/$UNIT.timer <<EOF
[Unit]
Description=Check the UltraLightBrowser line IPv6 address every hour

[Timer]
OnBootSec=30s
OnCalendar=hourly
Persistent=true

[Install]
WantedBy=timers.target
EOF
	systemctl daemon-reload
	systemctl enable --now $UNIT.timer
	systemctl start $UNIT.service
	status
}

uninstall_all() {
	[ "$(id -u)" = 0 ] || die "请用 root 运行（sudo）"
	systemctl disable --now $UNIT.timer 2>/dev/null || true
	if [ -r "$CONF" ]; then
		load_conf
		while read -r a; do [ -n "$a" ] && ip -6 addr del "$a/128" dev "$DEV" 2>/dev/null || true; done <"$STATE" 2>/dev/null || true
		nft delete table ip6 ulb_line 2>/dev/null || true
	fi
	rm -f /etc/systemd/system/$UNIT.service /etc/systemd/system/$UNIT.timer "$SELF" "$CONF" "$STATE"
	systemctl daemon-reload
	echo "已卸载。原来的固定地址没有改动。"
}

cmd=${1:-help}
shift || true
parse_args "$@"
case "$cmd" in
install) install_all ;;
apply) apply ;;
status) status ;;
uninstall) uninstall_all ;;
show)
	[ -n "$UUID" ] && [ -n "$PREFIX" ] || die "show 需要 --uuid 和 --prefix"
	addr_for "${PERIOD:-$(now_period)}"
	;;
*) sed -n '2,20p' "$0" ;;
esac
