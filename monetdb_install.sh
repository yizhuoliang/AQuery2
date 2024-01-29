#!/bin/bash

# Update the system
apt-get update
apt-get install -y sudo
apt-get install -y python3
apt-get install -y pip
apt-get install -y wget

# Install required packages
apt-get install -y software-properties-common

# Add the MonetDB repository to your system
apt-add-repository "deb https://dev.monetdb.org/downloads/deb/ $(lsb_release -cs) monetdb"
apt-add-repository "deb-src https://dev.monetdb.org/downloads/deb/ $(lsb_release -cs) monetdb"

# Add the MonetDB GPG key to trusted keys
wget --output-document=- https://www.monetdb.org/downloads/MonetDB-GPG-KEY | sudo apt-key add -

# Update the system again
apt-get update

# Install MonetDB
apt-get install -y monetdb5-sql monetdb-client

# Optional: Enable and start the MonetDB daemon
# sudo systemctl enable monetdbd
# sudo systemctl start monetdbd

echo "MonetDB installation completed!"

apt install -y python3 python3-pip clang-14 libmonetdbe-dev libmonetdb-client-dev monetdb5-sql-dev git
