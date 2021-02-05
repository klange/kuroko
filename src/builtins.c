const char krk_builtinsSrc[] = 
"# Please avoid using double quotes or escape sequences\n"
"# in this file to allow it to be easily converted to C.\n"
"class Helper():\n"
" '''You seem to already know how to use this.'''\n"
" def __call__(self,obj=None):\n"
"  if obj is not None:\n"
"   try:\n"
"    print(obj.__doc__)\n"
"   except:\n"
"    try:\n"
"     print(obj.__class__.__doc__)\n"
"    except:\n"
"     print('No docstring avaialble for', obj)\n"
"  else:\n"
"   from help import interactive\n"
"   interactive()\n"
" def __repr__(self):\n"
"  return 'Type help() for more help, or help(obj) to describe an object.'\n"
"\n"
"let help = Helper()\n"
"\n"
"class LicenseReader():\n"
" def __call__(self):\n"
"  from help import __licenseText\n"
"  print(__licenseText)\n"
" def __repr__(self):\n"
"  return 'Copyright 2020-2021 K. Lange <klange@toaruos.org>. Type `license()` for more information.'\n"
"\n"
"let license = LicenseReader()\n"
"\n"
"__builtins__.help = help\n"
"__builtins__.license = license\n"
"\n"
"# this works because `kuroko` is always a built-in\n"
"import kuroko\n"
"kuroko.module_paths = ['./']\n"
"if 'executable_path' in dir(kuroko):\n"
" let pathunits = kuroko.executable_path.split(kuroko.path_sep)[:-1]\n"
" let dirname  = pathunits[-1]\n"
" if dirname == 'bin':\n"
"  pathunits.pop(-1)\n"
"  pathunits.extend(['lib','kuroko',''])\n"
" else:\n"
"  pathunits.extend(['modules',''])\n"
" kuroko.module_paths.append(kuroko.path_sep.join(pathunits))\n"
"\n"
"return object()\n"
;
