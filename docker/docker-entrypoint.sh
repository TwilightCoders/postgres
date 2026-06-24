#!/usr/bin/env bash
# ProgreSQL container entrypoint — a trimmed analogue of the official postgres
# image's entrypoint. On first start it runs initdb, applies POSTGRES_* settings,
# and executes any /docker-entrypoint-initdb.d/*.{sql,sh}; thereafter it just
# starts the server against the existing $PGDATA.
set -Eeo pipefail

PGBIN=/usr/local/pgsql/bin
: "${PGDATA:=/var/lib/postgresql/data}"
: "${POSTGRES_USER:=postgres}"
: "${POSTGRES_DB:=$POSTGRES_USER}"

# When started as the server and running as root, fix ownership and drop to the
# unprivileged postgres user (PostgreSQL refuses to run as root).
if [ "$1" = 'postgres' ] && [ "$(id -u)" = '0' ]; then
	mkdir -p "$PGDATA"
	chown -R postgres:postgres "$PGDATA" /docker-entrypoint-initdb.d 2>/dev/null || true
	chmod 700 "$PGDATA" 2>/dev/null || true
	exec gosu postgres "$BASH_SOURCE" "$@"
fi

if [ "$1" = 'postgres' ] && [ ! -s "$PGDATA/PG_VERSION" ]; then
	# ---- first-time initialization ----
	if [ -z "${POSTGRES_PASSWORD:-}" ] && [ "${POSTGRES_HOST_AUTH_METHOD:-}" != 'trust' ]; then
		echo >&2 "ERROR: database is uninitialized and no superuser password is set."
		echo >&2 "       Set POSTGRES_PASSWORD to a non-empty value, or set"
		echo >&2 "       POSTGRES_HOST_AUTH_METHOD=trust to allow unauthenticated connections."
		exit 1
	fi

	if [ -n "${POSTGRES_PASSWORD:-}" ]; then
		pwfile="$(mktemp)"
		printf '%s' "$POSTGRES_PASSWORD" >"$pwfile"
		"$PGBIN/initdb" -D "$PGDATA" -U "$POSTGRES_USER" --pwfile="$pwfile" \
			--auth-host=scram-sha-256 --auth-local=trust >/dev/null
		rm -f "$pwfile"
		authline="host all all all scram-sha-256"
	else
		"$PGBIN/initdb" -D "$PGDATA" -U "$POSTGRES_USER" \
			--auth-host=trust --auth-local=trust >/dev/null
		authline="host all all all trust"
	fi

	echo "listen_addresses = '*'" >>"$PGDATA/postgresql.conf"
	echo "$authline" >>"$PGDATA/pg_hba.conf"

	# Start a private (socket-only) server to create the database and run init scripts.
	"$PGBIN/pg_ctl" -D "$PGDATA" \
		-o "-c listen_addresses='' -c unix_socket_directories=/tmp" -w start >/dev/null

	if [ "$POSTGRES_DB" != 'postgres' ]; then
		"$PGBIN/createdb" -h /tmp -U "$POSTGRES_USER" "$POSTGRES_DB" 2>/dev/null || true
	fi

	for f in /docker-entrypoint-initdb.d/*; do
		[ -e "$f" ] || continue
		case "$f" in
			*.sh)  echo ">> running $f";  . "$f" ;;
			*.sql) echo ">> applying $f"; "$PGBIN/psql" -h /tmp -v ON_ERROR_STOP=1 \
				-U "$POSTGRES_USER" -d "$POSTGRES_DB" -f "$f" ;;
			*)     echo ">> ignoring $f" ;;
		esac
	done

	"$PGBIN/pg_ctl" -D "$PGDATA" -w stop >/dev/null
	echo "ProgreSQL initialization complete."
fi

if [ "$1" = 'postgres' ]; then
	exec "$PGBIN/postgres" -D "$PGDATA"
fi

# Anything else (psql, bash, initdb, ...) runs as given.
exec "$@"
