# Running long experiments (tmux, crash-safe, resumable)

```bash
sudo apt install -y tmux                         # once
make run SETS="ablation"                         # start: tmux session 'qsq'
make attach                                      # watch (detach: Ctrl-b d; windows: Ctrl-b 0 run / Ctrl-b 1 monitor)
make progress                                    # done / total / speed / ETA, without attaching
```
After the ablation has been analysed:
```bash
make run SETS="main sens realism"
```

## What happens on disk
* Each simulation writes `data/v2/runs/<set-csv>/<job-id>.csv` via temp file + fsync + atomic rename.
  An existing file is always complete; a freeze loses only the runs in flight (≤ MAXJ runs).
* Re-running never repeats finished runs. After a freeze/reboot: `make resume`
  (or `bash tools/install_autoresume.sh` once, to resume automatically 60 s after boot).
* A run that fails or exceeds `JOB_TIMEOUT` (3600 s) is logged in `data/v2/logs/failed.txt`
  and retried once at the end of the pass and again on every resume.
* `data/v2/<name>.csv` are rebuilt from the per-run files by `tools/collect.py`
  (`make collect` / `make analyze`), at any time, also during a run.
* After every completed set the results are committed and pushed to GitHub
  (off-VM backup; disable with `GIT_BACKUP=0`).
* Only one runner per output folder (lock file); `make run` refuses to start a second session.

## Changing the design
A set is marked complete with `data/v2/DONE_<set>`. If `experiments/make_jobs.py` changes,
delete that marker (results already on disk are reused when job ids are unchanged).
