#!/usr/bin/env bash
# run_tmux.sh — start (or resume) the experiment queue inside tmux session "qsq".
#
#   bash tools/run_tmux.sh ablation main sens realism   # start a new queue
#   bash tools/run_tmux.sh                              # resume the stored queue
#   tmux attach -t qsq     (detach: Ctrl-b d)   window 0 = run, window 1 = monitor
#
# Closing the SSH window does not stop anything. After a VM freeze/reboot simply run
# this script again (or install the @reboot hook: bash tools/install_autoresume.sh):
# finished runs are kept on disk and are never repeated.
set -u
PROJ=$(cd "$(dirname "$0")/.." && pwd)
OUTDIR=${OUTDIR:-$PROJ/data/v2}; mkdir -p "$OUTDIR/logs"
SESSION=${SESSION:-qsq}
command -v tmux >/dev/null || { echo "install tmux: sudo apt install -y tmux"; exit 1; }
if tmux has-session -t "$SESSION" 2>/dev/null; then
  echo "session '$SESSION' already running -> tmux attach -t $SESSION"; exit 0
fi
[[ $# -gt 0 ]] && echo "$*" > "$OUTDIR/QUEUE"
[[ -s "$OUTDIR/QUEUE" ]] || { echo "usage: $0 <set> [<set>...]   (sets: smoke sanity ablation main realism sens)"; exit 1; }
LOG="$OUTDIR/logs/queue_$(date +%Y%m%d_%H%M%S).log"
tmux new-session -d -s "$SESSION" -n run -c "$PROJ" \
  "bash tools/run_queue.sh 2>&1 | tee -a '$LOG'; echo; echo 'queue finished — press Enter'; read"
tmux new-window -t "$SESSION" -n monitor -c "$PROJ" \
  "watch -n 60 'bash tools/progress.sh; echo; tail -n 5 $LOG'"
tmux select-window -t "$SESSION:run"
echo "started tmux session '$SESSION' (queue: $(cat "$OUTDIR/QUEUE"))"
echo "  attach : tmux attach -t $SESSION     detach: Ctrl-b d     switch window: Ctrl-b 0 / Ctrl-b 1"
echo "  log    : $LOG"
