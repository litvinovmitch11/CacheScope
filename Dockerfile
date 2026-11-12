FROM node:22-bookworm-slim AS ui
WORKDIR /ui
COPY ui/package*.json ./
RUN npm ci
COPY ui/ ./
RUN npm run build

# The project worker deliberately uses the same pinned Python/Torch frontend on
# every host. The final Ubuntu image receives both this interpreter and venv.
FROM python:3.11-bookworm AS torch-runtime
ENV PIP_DISABLE_PIP_VERSION_CHECK=1 \
    VIRTUAL_ENV=/opt/cachescope/venv
RUN python -m venv "$VIRTUAL_ENV"
COPY requirements-torch-demo.lock /tmp/requirements-torch-demo.lock
RUN "$VIRTUAL_ENV/bin/pip" install --no-cache-dir \
      -r /tmp/requirements-torch-demo.lock \
    && "$VIRTUAL_ENV/bin/python" -c \
      "import torch; from torch_mlir.fx import export_and_import; from torch_mlir.runtime import get_ranked_memref_descriptor"

FROM ubuntu:24.04 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive LLVM_ROOT=/usr/lib/llvm-22
COPY .github/ubuntu-amd64.sources /etc/apt/sources.list.d/ubuntu.sources
RUN apt-get update && apt-get install -y --no-install-recommends sudo g++ python3-numpy
WORKDIR /opt/cachescope
COPY cmake/LLVMVersion.cmake cmake/
COPY scripts/install-llvm-dependencies.sh scripts/
RUN scripts/install-llvm-dependencies.sh

FROM toolchain AS app
ARG CACHESCOPE_BUILD_TESTING=OFF
COPY . .
RUN cmake -S . -B build/app -G Ninja \
      -DMLIR_DIR=/usr/lib/llvm-22/lib/cmake/mlir \
      -DCMAKE_C_COMPILER=/usr/lib/llvm-22/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/lib/llvm-22/bin/clang++ \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=${CACHESCOPE_BUILD_TESTING} \
    && cmake --build build/app --parallel 3
COPY --from=ui /ui/dist ui/dist/
COPY --from=torch-runtime /usr/local/ /usr/local/
COPY --from=torch-runtime /opt/cachescope/venv/ /opt/cachescope/venv/
ENV CACHESCOPE_PYTHON=/opt/cachescope/venv/bin/python \
    PATH=/opt/cachescope/venv/bin:$PATH
RUN useradd --create-home cachescope
USER cachescope
EXPOSE 9055
CMD ["build/app/runner/cachescope_runner_server", "--host", "0.0.0.0", "--port", "9055", "--app-dir", "/opt/cachescope"]
