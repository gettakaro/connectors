# Python + Pillow image for the ARK icon spike (#813): PNG statistics and the contact sheet.
# The package reader (ark_uasset.py) is stdlib-only; Pillow is needed only to look at exported PNGs.
# Evidence tooling only: built locally, never pushed. Network is needed at build time only.
#
#   base image   python:3.12-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df
#   Pillow       11.3.0 cp312 manylinux wheel, pinned by hash
FROM python:3.12-slim-bookworm@sha256:935c3c5b12357a7e10caa9c1e6ca5692344d97cf229832ef584be2cec9fc03df
RUN printf '%s\n' \
      'pillow==11.3.0 --hash=sha256:676b2815362456b5b3216b4fd5bd89d362100dc6f4945154ff172e206a22c024' \
      > /tmp/requirements.txt \
 && pip install --no-cache-dir --require-hashes --only-binary=:all: -r /tmp/requirements.txt \
 && rm /tmp/requirements.txt
