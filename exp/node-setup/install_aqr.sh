#!/bin/bash

# Update the system
apt-get update
apt-get install -y sudo
apt-get install -y python3
apt-get install -y pip
apt-get install -y wget

# Install required packages
sudo apt-get install -y software-properties-common

# Add the MonetDB repository to your system
sudo apt-add-repository "deb https://dev.monetdb.org/downloads/deb/ $(lsb_release -cs) monetdb"
sudo apt-add-repository "deb-src https://dev.monetdb.org/downloads/deb/ $(lsb_release -cs) monetdb"

# Add the MonetDB GPG key to trusted keys
wget --output-document=- https://www.monetdb.org/downloads/MonetDB-GPG-KEY | sudo apt-key add -

# Update the system again
sudo apt-get update

# Install MonetDB
sudo apt-get install -y monetdb5-sql monetdb-client

# Optional: Enable and start the MonetDB daemon
# sudo systemctl enable monetdbd
# sudo systemctl start monetdbd

echo "MonetDB installation completed!"

sudo apt install -y python3 python3-pip clang-14 libmonetdbe-dev git

git clone https://github.com/yizhuoliang/AQuery2.git

cd AQuery2/

python3 -m pip install -r requirements.txt

git checkout aqr-exp-coulson

wget https://apt.llvm.org/llvm.sh
chmod +x llvm.sh
sudo ./llvm.sh 14

# install postgres
sudo apt install postgresql
