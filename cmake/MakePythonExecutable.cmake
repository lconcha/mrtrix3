# Creates within the bin/ sub-directory of the project build directory
#   a short Python executable that is used to run a Python command from the terminal
# Receives name of the command as ${CMDNAME}; output build directory as ${BUILDDIR}
set(BINPATH "${BUILDDIR}/temporary/python/${CMDNAME}")

set(BINPATH_CONTENTS
    "#!/usr/bin/python3\n"
    "# -*- coding: utf-8 -*-\n"
    "\n"
    "import importlib\n"
    "import os\n"
    "import sys\n"
    "\n"
    "mrtrix_lib_path = os.path.normpath(os.path.join(os.path.dirname(os.path.realpath(__file__)), os.pardir, 'lib'))\n"
    "sys.path.insert(0, mrtrix_lib_path)\n"
    "from mrtrix3.app import _execute\n"
)

# Three possible interfaces:
#   1. Standalone file residing in commands/
#   2. File stored in location commands/<cmdname>/<cmdname>.py, which will contain usage() and execute() functions
#   3. Two files stored at commands/<cmdname>/usage.py and commands/<cmdname>/execute.py, defining the two corresponding functions
# TODO Port population_template to type 3; both for readability and to ensure that it works
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CMDNAME}/__init__.py")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CMDNAME}/usage.py" AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CMDNAME}/execute.py")
        string(APPEND BINPATH_CONTENTS
            "module_usage = importlib.import_module('.usage', 'mrtrix3.commands.${CMDNAME}')\n"
            "module_execute = importlib.import_module('.execute', 'mrtrix3.commands.${CMDNAME}')\n"
            "_execute(module_usage.usage, module_execute.execute)\n"
        )
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CMDNAME}/${CMDNAME}.py")
        string(APPEND BINPATH_CONTENTS
            "module = importlib.import_module('.${CMDNAME}', 'mrtrix3.commands.${CMDNAME}')\n"
            "_execute(module.usage, module.execute)\n"
        )
    else()
        message(FATAL_ERROR "Malformed filesystem structure for Python command ${CMDNAME}")
    endif()
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${CMDNAME}.py")
    string(APPEND BINPATH_CONTENTS
        "module = importlib.import_module('${CMDNAME}')\n"
        "_execute(module.usage, module.execute)\n"
    )
else()
    message(FATAL_ERROR "Malformed filesystem structure for Python command ${CMDNAME}")
endif()

file(WRITE ${OUTPUT_DIR}/${CMDNAME} ${BINPATH_CONTENTS})
file(CHMOD ${OUTPUT_DIR}/${CMDNAME} FILE_PERMISSIONS
    OWNER_EXECUTE OWNER_WRITE OWNER_READ GROUP_EXECUTE GROUP_READ WORLD_EXECUTE WORLD_READ
)
