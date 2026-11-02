FROM ghcr.io/actions/actions-runner@sha256:e5496277be5d09bc968b3d64911b74e219ac4a3f2edce956a3ecf9271bea1ef4

USER root

ARG RUNNER_UID=1000
ARG RUNNER_GID=1000

RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        libasound2t64 \
        libatk-bridge2.0-0 \
        libatk1.0-0 \
        libatspi2.0-0 \
        libcairo2 \
        libdrm2 \
        libgbm1 \
        libnspr4 \
        libnss3 \
        libpango-1.0-0 \
        libxcomposite1 \
        libxdamage1 \
        libxfixes3 \
        libxkbcommon0 \
        libxrandr2 \
    && rm -rf /var/lib/apt/lists/* \
    && usermod --non-unique --uid "${RUNNER_UID}" --gid "${RUNNER_GID}" runner \
    && chown -R "${RUNNER_UID}:${RUNNER_GID}" /home/runner

USER runner
