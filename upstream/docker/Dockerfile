# FlareSim build image — ASWF base + upgraded CUDA toolkit (minimal install)
#
# Installs only the packages needed to compile CUDA code:
#   cuda-nvcc-12-x       — the nvcc compiler driver
#   cuda-cudart-devel-12-x — runtime headers + libcudart_static.a
#
# This keeps the image small (~100-180 MB extra) vs the full cuda-toolkit
# meta-package (~4 GB) which also pulls in Nsight, cuBLAS, cuFFT, etc.
#
# Platform mapping:
#   ASWF 2022 (CentOS 7 / Nuke 14)  → CUDA 12.4  (RHEL7 repo max)
#     adds sm_89 (Ada/RTX 4000) and sm_90 (Hopper); no sm_120 (Blackwell)
#   ASWF 2023-2025 (Rocky 8 / Nuke 15-17) → CUDA 12.8
#     adds sm_120 (Blackwell / RTX 5000 series)
#
# Build:
#   docker build --build-arg BASE_IMAGE=aswf/ci-vfxall:2024 \
#                -t flaresim-build:nuke16 docker/
#
# Nuke 13 (ASWF 2020 / GCC 6.3 / CUDA 10.2) is not covered here.

ARG BASE_IMAGE
FROM ${BASE_IMAGE}

RUN \
    if command -v dnf &>/dev/null; then \
        # Rocky Linux 8 (ASWF 2023-2025) — install CUDA 12.8 for sm_120 support
        dnf install -y 'dnf-plugins-core' && \
        dnf config-manager --add-repo \
            https://developer.download.nvidia.com/compute/cuda/repos/rhel8/x86_64/cuda-rhel8.repo && \
        dnf install -y cuda-nvcc-12-8 cuda-cudart-devel-12-8 && \
        dnf clean all; \
    else \
        # CentOS 7 (ASWF 2022) — CUDA 12.8 not in RHEL7 repo; use CUDA 12.4.
        # CentOS 7 reached EOL June 2024; redirect yum mirrors to vault.centos.org.
        sed -i 's/mirrorlist=/#mirrorlist=/g'        /etc/yum.repos.d/CentOS-*.repo && \
        sed -i 's|#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|g' \
                                                     /etc/yum.repos.d/CentOS-*.repo && \
        yum install -y 'yum-utils' && \
        yum-config-manager --add-repo \
            https://developer.download.nvidia.com/compute/cuda/repos/rhel7/x86_64/cuda-rhel7.repo && \
        yum install -y cuda-nvcc-12-4 cuda-cudart-devel-12-4 && \
        yum clean all; \
    fi && \
    rm -rf /var/cache/dnf /var/cache/yum

# Put the upgraded CUDA at the front of PATH so it takes priority over the
# older version already present in the base image.
# Both paths are listed; only the installed one will actually contain nvcc.
ENV PATH=/usr/local/cuda-12.8/bin:/usr/local/cuda-12.4/bin:${PATH}
ENV LD_LIBRARY_PATH=/usr/local/cuda-12.8/lib64:/usr/local/cuda-12.4/lib64:${LD_LIBRARY_PATH}
