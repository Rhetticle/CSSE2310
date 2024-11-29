#!/usr/bin/bash

cd /tmp
echo "Cloning Stockfish..."
git clone https://github.com/official-stockfish/Stockfish.git
cd Stockfish/src
make -j build
mv stockfish /usr/local/bin
cd /tmp
rm -rf Stockfish
echo "Done."
