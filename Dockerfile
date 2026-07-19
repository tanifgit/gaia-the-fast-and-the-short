ARG IMAGE=intersystems/iris-community:latest-em
FROM $IMAGE

WORKDIR /home/irisowner/dev
COPY . .

# Prebuild the native kernel during image build so that do ^RunScript never pays
# a compile cost. If a bind mount later shadows /home/irisowner/dev/src, the
# runtime in flux_runner.py transparently rebuilds fluxscan.so on first use.
# libdeflate is dlopen'd by the kernel at run time, so it is not on the link line.
USER root
RUN set -e; \
    if command -v gcc >/dev/null 2>&1; then \
        gcc -O3 -march=native -funroll-loops -fopenmp -fPIC -shared \
            src/fluxscan.c -ldl -lm -o src/fluxscan.so \
        || gcc -O3 -fopenmp -fPIC -shared \
            src/fluxscan.c -ldl -lm -o src/fluxscan.so; \
        chown irisowner:irisowner src/fluxscan.so; \
        echo "prebuilt src/fluxscan.so"; \
    else \
        echo "no gcc at build time; runtime path will handle it"; \
    fi

USER irisowner

ENV IRISUSERNAME="_SYSTEM"
ENV IRISPASSWORD="SYS"
ENV IRISNAMESPACE="USER"
ENV PYTHON_PATH=/usr/irissys/bin/
ENV PYTHONPATH=/home/irisowner/dev/src
ENV PATH="/usr/irissys/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/home/irisowner/bin"

RUN --mount=type=bind,src=.,dst=. \
    iris start IRIS && \
    iris merge IRIS merge.cpf && \
    iris session IRIS < iris.script && \
    iris stop IRIS quietly safely
