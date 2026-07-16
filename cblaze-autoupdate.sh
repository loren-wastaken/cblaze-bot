#!/bin/bash
set -e

REPO_DIR="/root/cblaze-bot"
SERVICE_NAME="cblaze"

cd "$REPO_DIR"

# Fetch remote refs without touching the working tree
git fetch origin

LOCAL_HASH=$(git rev-parse HEAD)
REMOTE_HASH=$(git rev-parse origin/main)

if [ "$LOCAL_HASH" == "$REMOTE_HASH" ]; then
    echo "[autoupdate] No new commits. Local == Remote ($LOCAL_HASH). Nothing to do."
    exit 0
fi

echo "[autoupdate] New commit detected. Local=$LOCAL_HASH Remote=$REMOTE_HASH"
echo "[autoupdate] Stopping $SERVICE_NAME..."
systemctl stop "$SERVICE_NAME"

echo "[autoupdate] Pulling latest changes..."
git pull origin main

echo "[autoupdate] Removing old build directory..."
rm -rf "$REPO_DIR/build"

echo "[autoupdate] Creating fresh build directory..."
mkdir -p "$REPO_DIR/build"
cd "$REPO_DIR/build"

echo "[autoupdate] Running cmake..."
cmake ..

echo "[autoupdate] Building..."
make

echo "[autoupdate] Restarting $SERVICE_NAME..."
systemctl start "$SERVICE_NAME"

echo "[autoupdate] Done. Now running commit $(git -C "$REPO_DIR" rev-parse HEAD)."
