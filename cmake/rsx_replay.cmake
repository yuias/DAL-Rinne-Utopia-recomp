# Pins the capture the rsx_replay test replays. The capture is game-derived and git-ignored; it is
# taken with dal_game --rsx-capture and copied to generated/rsx/captures/<name>.dalcap.
set(DAL_RSX_REPLAY_CAPTURE title)

# Filled in once a reference image has been accepted for the capture: the SHA-256 of
# generated/rsx/reference/<name>.png and the largest per-channel and mean differences (and the share of
# differing pixels) a replay may show against it. While empty, the test only checks the summary line.
set(DAL_RSX_REPLAY_REFERENCE_SHA256 "")
set(DAL_RSX_REPLAY_MAX_DIFF "")
set(DAL_RSX_REPLAY_MEAN_DIFF "")
set(DAL_RSX_REPLAY_DIFFERING_PERCENT "")
