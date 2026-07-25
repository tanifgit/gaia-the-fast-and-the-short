ARG IMAGE=intersystems/iris-community:latest-em
FROM $IMAGE

WORKDIR /home/irisowner/dev
COPY . .

# Stage the input archives to a container-local directory. Reading the .gz files
# from the compose bind mount (which proxies to the host filesystem) is ~3x
# slower per run than reading them from the container's own filesystem, even
# warm - the virtualized mount cannot serve pages at native speed. Copying to
# /tmp at build time means do ^RunScript reads from local storage. The runner
# prefers this directory and falls back to data/in if it is absent.
USER root
RUN mkdir -p /tmp/gaia_in && cp data/in/*.gz /tmp/gaia_in/ && \
    chown -R irisowner:irisowner /tmp/gaia_in

# Prebuild the native kernel during image build so that do ^RunScript never pays
# a compile cost. If a bind mount later shadows /home/irisowner/dev/src, the
# runtime in flux_runner.py transparently rebuilds fluxscan.so on first use.
# libdeflate is dlopen'd by the kernel at run time, so it is not on the link line.
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
