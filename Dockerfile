FROM node:22-bookworm-slim AS ui
WORKDIR /ui
COPY ui/package*.json ./
RUN npm ci
COPY ui/ ./
RUN npm run build

FROM ubuntu:24.04 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive LLVM_ROOT=/usr/lib/llvm-22
RUN apt-get update && apt-get install -y --no-install-recommends sudo g++ python3-numpy
WORKDIR /opt/cachescope
COPY cmake/LLVMVersion.cmake cmake/
COPY scripts/install-llvm-dependencies.sh scripts/
RUN scripts/install-llvm-dependencies.sh

FROM toolchain AS app
COPY . .
RUN cmake -S . -B build/app -G Ninja \
      -DMLIR_DIR=/usr/lib/llvm-22/lib/cmake/mlir \
      -DCMAKE_C_COMPILER=/usr/lib/llvm-22/bin/clang \
      -DCMAKE_CXX_COMPILER=/usr/lib/llvm-22/bin/clang++ \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON \
    && cmake --build build/app --parallel 3
COPY --from=ui /ui/dist ui/dist/
RUN useradd --create-home cachescope
USER cachescope
EXPOSE 9055
CMD ["build/app/runner/cachescope_runner_server", "--host", "0.0.0.0", "--port", "9055", "--app-dir", "/opt/cachescope"]
