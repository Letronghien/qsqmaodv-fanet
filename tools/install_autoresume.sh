#!/usr/bin/env bash
# install_autoresume.sh — optional: resume the queue automatically after a reboot.
#   bash tools/install_autoresume.sh          # add the @reboot crontab line
#   bash tools/install_autoresume.sh remove   # remove it
PROJ=$(cd "$(dirname "$0")/.." && pwd)
LINE="@reboot sleep 60 && cd $PROJ && [ -s data/v2/QUEUE ] && bash tools/run_tmux.sh >> data/v2/logs/autoresume.log 2>&1"
if [[ "${1:-}" == remove ]]; then
  crontab -l 2>/dev/null | grep -vF "tools/run_tmux.sh" | crontab -; echo "removed"; exit 0
fi
( crontab -l 2>/dev/null | grep -vF "tools/run_tmux.sh"; echo "$LINE" ) | crontab -
echo "installed: $LINE"
echo "The queue in data/v2/QUEUE resumes 60 s after every boot (finished sets are skipped)."
