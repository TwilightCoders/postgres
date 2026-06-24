# ProgreSQL — PostgreSQL 18 fork with cross-partition GLOBAL (spanning) indexes.
# Multi-stage: build the fork + full contrib from a pinned, checksum-verified
# release tarball, then ship a slim runtime with a postgres-style entrypoint.
#
#   docker build -t ghcr.io/twilightcoders/progresql:18.3-0.2.0 \
#       --build-arg PROGRESQL_VERSION=18.3-0.2.0 \
#       --build-arg PROGRESQL_SHA256=<sha> docker/
#
#   docker run -e POSTGRES_PASSWORD=secret -p 5432:5432 \
#       ghcr.io/twilightcoders/progresql:18

ARG PROGRESQL_VERSION=18.3-0.2.0
ARG PROGRESQL_SHA256=4cab230109a9debf8e772de0db8d3d6bcd530c323e19c1ad0bb480e99e3799a4

FROM debian:bookworm AS builder
ARG PROGRESQL_VERSION
ARG PROGRESQL_SHA256
RUN apt-get update && apt-get install -y --no-install-recommends \
		ca-certificates curl gcc make bison flex perl pkg-config \
		libssl-dev libreadline-dev zlib1g-dev \
	&& rm -rf /var/lib/apt/lists/*
WORKDIR /build
RUN curl -fsSL "https://github.com/TwilightCoders/progresql/archive/refs/tags/v${PROGRESQL_VERSION}.tar.gz" -o src.tgz \
	&& echo "${PROGRESQL_SHA256}  src.tgz" | sha256sum -c - \
	&& tar xzf src.tgz --strip-components=1 \
	&& rm src.tgz
# Lean configure (parity with the Homebrew formula) + the full contrib module set.
RUN ./configure --prefix=/usr/local/pgsql --with-openssl --with-readline --without-icu \
	&& make -j"$(nproc)" \
	&& make install \
	&& make -C contrib -j"$(nproc)" \
	&& make -C contrib install \
	&& strip /usr/local/pgsql/bin/* 2>/dev/null || true

FROM debian:bookworm-slim
LABEL org.opencontainers.image.title="ProgreSQL" \
	org.opencontainers.image.description="PostgreSQL 18 fork with cross-partition GLOBAL (spanning) unique/PK indexes" \
	org.opencontainers.image.source="https://github.com/TwilightCoders/progresql" \
	org.opencontainers.image.licenses="PostgreSQL"
RUN apt-get update && apt-get install -y --no-install-recommends \
		libssl3 libreadline8 zlib1g locales gosu \
	&& rm -rf /var/lib/apt/lists/* \
	&& localedef -i en_US -c -f UTF-8 -A /usr/share/locale/locale.alias en_US.UTF-8
ENV LANG=en_US.utf8 \
	PATH=/usr/local/pgsql/bin:$PATH \
	PGDATA=/var/lib/postgresql/data
COPY --from=builder /usr/local/pgsql /usr/local/pgsql
RUN groupadd -r postgres --gid=999 \
	&& useradd -r -g postgres --uid=999 -m -d /var/lib/postgresql postgres \
	&& mkdir -p /var/lib/postgresql/data /docker-entrypoint-initdb.d \
	&& chown -R postgres:postgres /var/lib/postgresql
COPY docker-entrypoint.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/docker-entrypoint.sh
VOLUME /var/lib/postgresql/data
EXPOSE 5432
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["postgres"]
