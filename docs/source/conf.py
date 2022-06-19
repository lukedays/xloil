# Configuration file for the Sphinx documentation builder.
#
# This file only contains a selection of the most common options. For a full
# list see the documentation:
# https://www.sphinx-doc.org/en/master/usage/configuration.html

# -- Path setup --------------------------------------------------------------

# If extensions (or modules to document with autodoc) are in another directory,
# add these directories to sys.path here. If the directory is relative to the
# documentation root, use os.path.abspath to make it absolute, like shown here.
#
# import os
# import sys
# sys.path.insert(0, os.path.abspath('.'))

import os
import sys
from pathlib import Path

soln_dir = Path(os.environ["XLOIL_SOLN_DIR"])
bin_dir = Path(os.environ["XLOIL_BIN_DIR"])

print("xlOil solution directory: ", str(soln_dir))
sys.path.append(str(bin_dir))


# -- Project information -----------------------------------------------------

project = 'xlOil'
copyright = '2022, Steven Cunnane'
author = 'Steven Cunnane'

# The full version, including alpha/beta/rc tags
release = (soln_dir / "Version.txt").read_text()

# -- General configuration ---------------------------------------------------


# Add any Sphinx extension module names here, as strings. They can be
# extensions coming with Sphinx (named 'sphinx.ext.*') or your custom
# ones.
extensions = [
    "sphinx.ext.autodoc", 
    "sphinx.ext.autosummary", 
    "sphinx.ext.autosectionlabel", 
    "numpydoc",
    'autodocsumm'
    ]

# Avoid duplicate label warnings from autosectionlabel
suppress_warnings = ['autosectionlabel.*']

# True to prefix each section label with the name of the document it is in, 
# followed by a colon. For example, index:Introduction for a section called 
# Introduction that appears in document index.rst.
autosectionlabel_prefix_document = True

# Add any paths that contain templates here, relative to this directory.
templates_path = ['_templates']

# List of patterns, relative to source directory, that match files and
# directories to ignore when looking for source files.
# This pattern also affects html_static_path and html_extra_path.
exclude_patterns = []


# -- Options for HTML output -------------------------------------------------

# The theme to use for HTML and HTML Help pages.  See the documentation for
# a list of builtin themes.
#
html_theme = 'bizstyle'

# Add any paths that contain custom static files (such as style sheets) here,
# relative to this directory. They are copied after the builtin static files,
# so a file named "default.css" will overwrite the builtin "default.css".
html_static_path = []# ['_static']

# A list of paths that contain extra files not directly related to the documentation, 
# such as robots.txt or .htaccess. Relative paths are taken as relative to the 
# configuration directory. They are copied to the output directory.
#html_extra_path = ['../build/doxygen']

autodoc_default_flags = ['members']

autosummary_generate = False

# See https://stackoverflow.com/questions/34216659/
numpydoc_show_class_members=False

#
# Required for readthedocs build as the master_doc seems to default to 'contents' there
# Locally build is fine without this
#
master_doc = 'index'

# -- Generate examples file ---------------------------------------------------

import zipfile
from zipfile import ZipFile

try: os.makedirs('_build')
except FileExistsError: pass

zipObj = ZipFile('_build/xlOilExamples.zip', 'w', compression=zipfile.ZIP_BZIP2)
 
zipObj.write(soln_dir / "tests" / "TestSheets" / "PythonTest.xlsm", "PythonTest.xlsm")
zipObj.write(soln_dir / "tests" / "TestSheets" / "PythonTest.py", "PythonTest.py")
zipObj.write(soln_dir / "tests" / "TestSheets" / "TestModule.py", "TestModule.py")
zipObj.write(soln_dir / "tests" / "TestSheets" / "TestSQL.xlsx", "TestSQL.xlsx")
zipObj.write(soln_dir / "tests" / "TestSheets" / "TestUtils.xlsx", "TestUtils.xlsx")
 
zipObj.close()
