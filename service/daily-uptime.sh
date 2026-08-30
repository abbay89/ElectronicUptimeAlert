#!/bin/bash
# Daily uptime accumulator - runs on STB (Armbian) via cron every 5 min.
# Accumulates device online-time per day (seconds) into Firebase:
#   uptimeAlert/daily/<YYYY-MM-DD>
# Online detection = fresh last_heartbeat (< 3x heartbeat interval).
# Checkpoint data kept at uptimeAlert/service with epoch seconds.
BASE="https://thiefdetectorapp-default-rtdb.asia-southeast1.firebasedatabase.app/uptimeAlert"
APIKEY="AIzaSyCwgPIXYmb1X265MAMnblvhuLH-F397HuY"
EMAIL="abbay89@gmail.com"
PASS="Pangeran89"
TICK=300
FRESH_MAX=$((TICK * 2))                 # heartbeat max age 10 min when healthy (60s hb)

TOKEN=$(curl -s -X POST "https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=$APIKEY" -H "Content-Type: application/json" -d "{\"email\":\"$EMAIL\",\"password\":\"$PASS\",\"returnSecureToken\":true}" | jq -r .idToken)
if [ -z "$TOKEN" ] || [ "$TOKEN" = "null" ]; then
  echo "$(date '+%F %T') AUTH FAIL"
  exit 1
fi

STATUS=$(curl -s "$BASE/status.json?auth=$TOKEN")
if [ -z "$STATUS" ] || [ "$STATUS" = "null" ]; then
  echo "$(date '+%F %T') NO STATUS"
  exit 0
fi

UPT=$(echo "$STATUS" | jq -r '.uptime_seconds // 0')
BOOT=$(echo "$STATUS" | jq -r '.last_boot // ""')
HB=$(echo "$STATUS" | jq -r '.last_heartbeat // ""')

SVC=$(curl -s "$BASE/service.json?auth=$TOKEN")
PREV_NOW=$(echo "$SVC" | jq -r '.now // empty')
PREV_BOOT=$(echo "$SVC" | jq -r '.boot // empty')

NOW=$(date +%s)
HBE=0
if [ -n "$HB" ]; then HBE=$(date -d "$HB" +%s 2>/dev/null || echo 0); fi

ADD=0
if [ -n "$PREV_NOW" ] && [ "$HBE" -gt 0 ] && [ $(( NOW - HBE )) -le $FRESH_MAX ]; then
  ELAPSED=$(( NOW - PREV_NOW ))
  if [ "$ELAPSED" -gt 0 ]; then
    if [ "$BOOT" = "$PREV_BOOT" ]; then
      ADD=$ELAPSED
      [ "$ADD" -gt "$TICK" ] && ADD=$TICK
    else
      ADD=$UPT
      [ "$ADD" -gt "$ELAPSED" ] && ADD=$ELAPSED
    fi
  fi
fi

DAY=$(TZ=Asia/Jakarta date +%F)

if [ "$ADD" -gt 0 ]; then
  CUR=$(curl -s "$BASE/daily/$DAY.json?auth=$TOKEN" | jq -r 'if .==null then 0 else . end')
  NEW=$(( CUR + ADD ))
  curl -s -X PATCH "$BASE/daily.json?auth=$TOKEN" -H "Content-Type: application/json" -d "{\"$DAY\":$NEW}" > /dev/null
  echo "$(date '+%F %T') $DAY +${ADD}s => ${NEW}s"
else
  echo "$(date '+%F %T') $DAY stale/offline +0s"
fi

curl -s -X PATCH "$BASE/service.json?auth=$TOKEN" -H "Content-Type: application/json" -d "{\"now\":$NOW,\"boot\":\"$BOOT\"}" > /dev/null