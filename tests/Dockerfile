# Ambiente di prova su Linux per ping_core (gcc + iproute2 per simulare la latenza con tc netem)
FROM gcc:13-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends iproute2 && rm -rf /var/lib/apt/lists/*
