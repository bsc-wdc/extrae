#!/bin/sh

export EXTRAE_HOME=@sub_PREFIXDIR@
source ${EXTRAE_HOME}/etc/extrae.sh

${EXTRAE_HOME}/bin/extrae -config extrae.xml ./pi

#${EXTRAE_HOME}/bin/mpi2prv -e pi -f TRACE.mpits -s TRACE.sym -o pi.prv
