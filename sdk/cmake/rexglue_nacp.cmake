# ssasr-nx (2026-11-25): nacptool run without a shell. rexglue_switch.cmake's packaging ran it as a POST_BUILD command,
# which Ninja joins with the link step into one `cmd.exe /C "..."` line: a title with an "&" ("Sonic & SEGA All-Stars
# Racing") was split there and nacptool got "Sonic" alone (it printed its usage and the build failed). The title comes
# here with "&" written as __AMP__ (no shell sees an "&") and goes to nacptool through execute_process, as one argument.
#   cmake -DNACPTOOL=<nacptool> -DNAME=<title, & as __AMP__> -DAUTHOR=<author> -DVERSION=<version> -DOUT=<file.nacp>
#         -P rexglue_nacp.cmake
string(REPLACE "__AMP__" "&" NAME "${NAME}")
execute_process(COMMAND "${NACPTOOL}" --create "${NAME}" "${AUTHOR}" "${VERSION}" "${OUT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(FATAL_ERROR "nacptool failed (${rc}) for \"${NAME}\"")
endif()
