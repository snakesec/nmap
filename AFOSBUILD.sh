apt update
apt install python3-build

./configure --with-libpcre=included --with-libssh2=included --with-libdnet=included --with-liblua=included --with-liblinear=included --without-zenmap --without-ndiff

if [ $? -eq 0 ]
then
  # Result is OK! Just continue...
  echo "Configure... PASS!"
else
  # houston we have a problem
  exit 1
fi

make -j2 

if [ $? -eq 0 ]
then
  # Result is OK! Just continue...
  echo "Make... PASS!"
else
  # houston we have a problem
  exit 1
fi

make install

if [ $? -eq 0 ]
then
  # Result is OK! Just continue...
  echo "Make install... PASS!"
else
  # houston we have a problem
  exit 1
fi
