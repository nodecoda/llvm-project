import os
import lit.formats

config.name = "gisel-fixtures"
config.test_format = lit.formats.ShTest(execute_external=False)
config.suffixes = [".ll", ".mir"]
config.test_source_root = os.path.dirname(os.path.abspath(__file__))
config.test_exec_root = os.path.join(config.test_source_root, "_out")

llc = lit_config.params.get("llc")
filecheck = lit_config.params.get("filecheck")
if not llc or not filecheck:
    lit_config.fatal("pass --param llc=... --param filecheck=...")

bindir = os.path.dirname(os.path.abspath(llc))
not_tool = lit_config.params.get("not") or os.path.join(bindir, "not")
if not os.path.exists(not_tool):
    lit_config.fatal("cannot find 'not'; pass --param not=...")

config.substitutions.append(("%llc", llc))
config.substitutions.append(("%FileCheck", filecheck))
config.substitutions.append(("%not", not_tool))
