#!/usr/bin/env bash
# Run once with sudo; Azure registration is performed separately without sudo.
set -euo pipefail
if [[ $EUID != 0 ]]; then echo 'Run this script with sudo.' >&2; exit 1; fi
agent_user=${1:?Pass the local build-agent username}
agent_home=$(getent passwd "$agent_user" | cut -d: -f6)
if [[ -z "$agent_home" || $(id -u "$agent_user") == 0 ]]; then
    echo 'Choose an existing non-root agent user.' >&2; exit 1
fi
. /etc/os-release
ubuntu_release=${UBUNTU_CODENAME:-${VERSION_CODENAME:-}}
case "$ubuntu_release" in noble|jammy) ;; *) echo 'Expected Ubuntu 22.04/24.04 or the corresponding Mint release.' >&2; exit 1;; esac
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y ca-certificates curl git build-essential python3 python3-dev python3-venv python-is-python3 \
    python3-tk flex bison gperf cmake ninja-build ccache libffi-dev libssl-dev libusb-1.0-0 \
    libegl1 libopengl0 libgl1 libglib2.0-0 libfontconfig1 libdbus-1-3 libxkbcommon-x11-0 libxcb-cursor0 libicu-dev
install -d -m 0755 /etc/apt/keyrings
curl --fail --silent --show-error --location https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
chmod 0644 /etc/apt/keyrings/docker.asc
cat > /etc/apt/sources.list.d/docker.sources <<EOF
Types: deb
URIs: https://download.docker.com/linux/ubuntu
Suites: $ubuntu_release
Components: stable
Architectures: $(dpkg --print-architecture)
Signed-By: /etc/apt/keyrings/docker.asc
EOF
apt-get update -qq
apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
usermod -aG docker "$agent_user"
install -d -m 0755 -o "$agent_user" -g "$(id -gn "$agent_user")" "$agent_home/kms-build-agent"
cat > /etc/systemd/system/kms-azure-agent.service <<EOF
[Unit]
Description=KMSwitcher Azure Pipelines Linux agent
Wants=network-online.target
After=network-online.target docker.service
Requires=docker.service
ConditionPathExists=$agent_home/kms-build-agent/.ready

[Service]
User=$agent_user
Group=$(id -gn "$agent_user")
SupplementaryGroups=docker
WorkingDirectory=$agent_home/kms-build-agent
ExecStart=$agent_home/kms-build-agent/run.sh
Restart=always
RestartSec=15
KillSignal=SIGINT
TimeoutStopSec=300
Nice=10
CPUQuota=200%
MemoryHigh=5G
MemoryMax=6G
IOWeight=50
Environment=AGENT_TOOLSDIRECTORY=$agent_home/kms-build-agent/_work/_tool

[Install]
WantedBy=multi-user.target
EOF
cat > /etc/systemd/system/kms-azure-agent.path <<EOF
[Unit]
Description=Start the KMSwitcher agent after registration

[Path]
PathExists=$agent_home/kms-build-agent/.ready
Unit=kms-azure-agent.service

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now docker.service kms-azure-agent.path
systemctl enable kms-azure-agent.service
docker run --rm --cpus=2 --memory=6g hello-world
echo 'Host setup complete. The agent will start after registration is finished.'
