# cmake -DAR=<ar> -DMRI=<script> -DOUT=<archive> -P merge.cmake
# Merges static libraries into one archive with an MRI script (GNU ar and
# llvm-ar both read them). (As Punch-Out's player kit.)
file(REMOVE "${OUT}")
execute_process(COMMAND "${AR}" -M INPUT_FILE "${MRI}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "merging the player kit's libraries failed (${result})")
endif()
