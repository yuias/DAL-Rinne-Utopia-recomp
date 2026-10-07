# Pins the movie_decode test: dal_moviedump decodes the first frames of one game movie and compares
# them with the values below. The movie is game-derived and read in place from the game directory.
set(DAL_MOVIE_DECODE_MOVIE "PS3_GAME/USRDIR/Movie/ch_logo.pam")  # relative to DAL_GAME_DIR
set(DAL_MOVIE_DECODE_FRAMES 60)
set(DAL_MOVIE_DECODE_EXPECTED_FRAMES 60)

# SHA-256 of the decoder's Y, U and V planes (tightly packed, in that order) per output frame, as
# "<frame index>:<hash>". H.264 decoding is bit-exact, so these hold for any conforming FFmpeg build
# and thread count. While the list is empty, only the frame count is checked.
set(DAL_MOVIE_DECODE_YUV_SHA256
  "0:6d37aea2ada66b35c9d87d9e56038c9107da973e477ddf9f3ab57b58e71858a9"
  "30:26da0581a45d0d75b4ebea4d6155778d67f3d4d7eee28b28591b71ee9107ec72"
  "59:b9d1e992838b5f416f5dfd543e7f4f8277881b56c33cd480350dbbe27b5449d8"
)

# The same for the ARGB conversion (BT.709 from the stream info, alpha 0xff, tightly packed). swscale
# output was identical across runs and thread counts for this build; a different FFmpeg build may
# round differently, in which case re-pin these (the YUV hashes above stay valid).
set(DAL_MOVIE_DECODE_RGB_SHA256
  "0:18deb2d0d1d82b52c1d2f86a334e57511758a1fdfe4556b3573d44e6010fb3a6"
  "30:e9467c1ecabcf1110a3319f7c29e2699143985f470c88b36f118052d2c4b1626"
  "59:db19ebc4ec0037fef86fa5ea7b536ccfceb390be64ebea3e44fbfe1cdc6015da"
)
