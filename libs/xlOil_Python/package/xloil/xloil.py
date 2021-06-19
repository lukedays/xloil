import inspect
import functools
import importlib
import importlib.util
import importlib.abc
import typing
import os
import sys
import traceback
from .type_converters import *

#
# If the xloil_core module can be found, we are being called from an xlOil
# embedded interpreter, so we go ahead and import the module. Otherwise we
# define skeletons of the imported types to support type-checking, linting,
# auto-completion and documentation.
#
if importlib.util.find_spec("xloil_core") is not None:
    import xloil_core         # pylint: disable=import-error
    from xloil_core import (  # pylint: disable=import-error
        CellError, FuncOpts, Range, ExcelArray, in_wizard, log,
        event, cache, RtdServer, RtdPublisher, get_event_loop,
        register_functions, deregister_functions,
        create_ribbon, RibbonUI, run_later, 
        get_excel_state, Caller,
        CannotConvert, 
        from_excel_date,
        insert_cell_image)

else:
    from .shadow_core import *
    from .shadow_core import (_CustomReturn, _CustomConverter)
    

"""
Tag used to mark functions to register with Excel. It is added 
by the xloil.func decorator to the target func's __dict__
"""
_META_TAG = "_xloil_func_"


class Arg:
    """
    Holds the description of a function argument. Can be used with the 'func'
    decorator to specify the argument description.
    """
    def __init__(self, name, help="", typeof=None, default=None, is_keywords=False):
        """
        Parameters
        ----------

        name: str
            The name of the argument which appears in Excel's function wizard
        help: str, optional
            Help string to display in the function wizard
        typeof: object, optional
            Selects the type converter used to pass the argument value
        default: object, optional
            A default value to pass if the argument is not specified in Excel
        is_keywords: bool, optional
            Denotes the special kwargs argument. xlOil will expect a two-column array
            in Excel which it will interpret as key, value pairs and convert to a
            dictionary.
        """

        self.typeof = typeof
        self.name = str(name)
        self.help = help
        self.default = default
        self.is_keywords = is_keywords

    @property
    def has_default(self):
        """ 
        Since 'None' is a fairly likely default value, this property indicates 
        whether there was a user-specified default
        """
        return self.default is not inspect._empty

def _function_argspec(func):
    """
    Returns a list of Arg for a given function which describe
    the function's arguments
    """
    sig = inspect.signature(func)
    params = sig.parameters
    args = []
    for name, param in params.items():
        if param.kind == param.POSITIONAL_ONLY or param.kind == param.POSITIONAL_OR_KEYWORD:
            spec = Arg(name, default=param.default)
            anno = param.annotation
            if anno is not param.empty:
                spec.typeof = anno
                # Add a little help string based on the type annotation
                if isinstance(anno, type):
                    spec.help = f"({anno.__name__})"
                else:
                    spec.help = f"({str(anno)})"
            args.append(spec)
        elif param.kind == param.VAR_POSITIONAL:
             raise Exception(f"Unhandled argument type positional for {name}")
        elif param.kind == param.VAR_KEYWORD: # can type annotions make any sense here?
            args.append(Arg(name, is_keywords=True))
        else: 
            raise Exception(f"Unhandled argument type for {name}")
    return args, sig.return_annotation


class FuncDescription:
    """
    Used to create the description of a worksheet function to register. 
    External users would not typically use this class directly.
    """
    def __init__(self, func):
        self._func = func
        self.args, self.return_type = _function_argspec(func)
        self.name = func.__name__
        self.help = func.__doc__
        self.is_async = False
        self.rtd = None
        self.macro = False
        self.threaded = False
        self.volatile = False
        self.local = None

    def create_holder(self):
        """
        Creates a core object which holds function info, argument converters,
        and a reference to the function object
        """
        
        info = xloil_core.FuncInfo()
        info.args = [xloil_core.FuncArg(x.name, x.help) for x in self.args]
        info.name = self.name
        
        if self.help:
            info.help = self.help
            
        has_kwargs = any(self.args) and self.args[-1].is_keywords

        holder = xloil_core.FuncHolder(info, self._func, has_kwargs)
        
        # Set the arg converters based on the typeof provided for 
        # each argument. If 'typeof' is a xloil typeconverter object
        # it's passed through.  If it is a general python type, we
        # attempt to create a suitable typeconverter
        for i, arg_info in enumerate(self.args):
            if arg_info.is_keywords:
                continue

            # Determine the internal C++ arg converter to run on the Excel values
            # before they are passed to python.  
            converter = None
            this_arg = info.args[i]
            arg_type = arg_info.typeof

            # If a typing annotation is None or not a type, ignore it.
            # The default option is the generic converter which gives a python 
            # type based on the provided Excel type
            if not isinstance(arg_type, type):
                converter = xloil_core.Read_object()
            else:
                # The ordering of these cases is based on presumed likeliness.
                # First try an internal converter e.g. Read_str, Read_float, etc.
                converter = get_internal_converter(arg_type.__name__)

                # xloil_core.Range is special: the only core class in typing annotations
                if arg_type is Range:
                    this_arg.allow_range = True

                # If internal converter was found, nothing more to do
                if converter is not None:
                    pass
                # A designated xloil @converter type contains the internal converter
                elif is_type_converter(arg_type):
                    converter, this_arg.allow_range = unpack_type_converter(arg_type)
                # ExcelValue is just the explicit generic type, so do nothing
                elif arg_type is ExcelValue:
                    pass 
                elif arg_type is AllowRange:
                    converter = xloil_core.Read_object(), 
                    this_arg.allow_range = True
                # Attempt to find a registered user-converter, otherwise assume the object
                # should be read from the cache 
                else:
                    converter = arg_converters.get_converter(arg_type)
                    if converter is None:
                        converter = xloil_core.Read_Cache()
            log(f"Func {info.name}, arg {arg_info.name} using converter {type(converter)}", level="trace")
            if arg_info.has_default:
                this_arg.optional = True
                holder.set_arg_type_defaulted(i, converter, arg_info.default)
            else:
                holder.set_arg_type(i, converter)

        if self.return_type is not inspect._empty:
            ret_type = self.return_type
            if isinstance(ret_type, type):

                ret_con = None
                if is_type_converter(ret_type):
                    ret_con, _ = unpack_type_converter(ret_type)
                else:
                    ret_con = return_converters.create_returner(ret_type)

                    if ret_con is None:
                        ret_con = get_internal_converter(ret_type.__name__, read_excel_value=False)

                    if ret_con is None:
                        ret_con = Return_object()

                holder.return_converter = ret_con

        # RTD-async is default unless rtd=False was explicitly specified.
        holder.rtd_async = self.is_async and (self.rtd is not False)
        holder.native_async = self.is_async and not holder.rtd_async

        holder.local = True if (self.local is None and not holder.native_async) else self.local

        func_options = ((FuncOpts.Macro if self.macro else 0)
                        | (FuncOpts.ThreadSafe if self.threaded else 0)
                        | (FuncOpts.Volatile if self.volatile else 0))

        if holder.local:
            if func_options != 0:
                log(f"Ignoring func options for local function {self.name}", level='info')
        else:
            holder.set_opts(func_options)
        return holder


def _get_meta(fn):
    return fn.__dict__.get(_META_TAG, None)


def _create_event_loop():
    import asyncio
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    return loop

def async_wrapper(fn):
    """
    Wraps an async function or generator with a function which runs that generator on the thread's
    event loop. The wrapped function requires an 'xloil_thread_context' argument which provides a 
    callback object to return a result. xlOil will pass this object automatically to functions 
    declared async.

    This function is used by the `func` decorator and generally should not be invoked
    directly.
    """

    import asyncio

    @functools.wraps(fn)
    def synchronised(*args, xloil_thread_context, **kwargs):

        loop = get_event_loop()
        ctx = xloil_thread_context

        async def run_async():
            result = None
            try:
                # TODO: is inspect.isasyncgenfunction expensive?
                if inspect.isasyncgenfunction(fn):
                    async for result in fn(*args, **kwargs):
                        ctx.set_result(result)
                else:
                    result = await fn(*args, **kwargs)
                    ctx.set_result(result)
            except (asyncio.CancelledError, StopAsyncIteration):
                ctx.set_done()
                raise
            except Exception as e:
                ctx.set_result(str(e) + ": " + traceback.format_exc())
                
            ctx.set_done()
            
        ctx.set_task(asyncio.run_coroutine_threadsafe(run_async(), loop))

    return synchronised    

def _pump_message_loop(loop, timeout):
    """
    Called internally to run the asyncio message loop.
    """
    import asyncio

    async def wait():
        await asyncio.sleep(timeout)
    
    loop.run_until_complete(wait())


def func(fn=None, 
         name=None, 
         help=None, 
         args=None,
         group=None, 
         local=None,
         is_async=False, 
         rtd=None,
         macro=False, 
         threaded=False, 
         volatile=False):
    """ 
    Decorator which tells xlOil to register the function in Excel. 
    If arguments are annotated using 'typing' annotations, xlOil will attempt to 
    convert values received from Excel to the specfied type, raising an exception 
    if this is not possible. The currently available types are

    * **int**
    * **float**
    * **str**: Note this disables cache lookup
    * **bool**
    * **numpy arrays**: see Array
    * **CellError**: Excel has various error types such as #NUM!, #N/A!, etc.
    * **None**: if the argument points to an empty cell
    * **cached objects**
    * **datetime.date**
    * **datetime.datetime**
    * **dict / kwargs**: this converter expects a two column array of key/value pairs

    If no annotations are specified, xlOil will pass a type from the first eight above types
    based on the value provided from Excel.

    If a parameter default is given in the function signature, that parameter becomes optional in 
    the declared Excel function.

    Parameters
    ----------

    name: str
        Overrides the funtion name registered with Excel otherwise the function's 
        declared name is used.
    help: str
        Overrides the help shown in the function wizard otherwise the function's 
        doc-string is used. The wizard cannot display strings longer than 255 chars.
        Longer help string can be retrieved with `xloHelp`
    args: dict
        A dictionary with key names matching function arguments and values specifying
        information for that argument. The information can be a string, which is 
        interpreted as the help to display in the function wizard or in can be an 
        xloil.Arg object which can contain defaults, help and type information. 
    group: str
        Specifes a category of functions in Excel's function wizard under which
        this function should be placed.
    local: bool
        Functions in a workbook-linked module, e.g. Book1.py, default to 
        workbook-level scope (i.e. not usable outside that workbook) itself. You 
        can override this behaviour with this parameter. It has no effect outside 
        workbook-linked modules.
    macro: bool
        If True, registers the function as Macro Type. This grants the function
        extra priveledges, such as the ability to see un-calced cells and 
        call the full range of Excel.Application functions. Functions which will
        be invoked as Excel macros, i.e. not functions appearing in a cell, should
        be declared with this attribute.
    is_async: bool
        Registers the function as asynchronous. It's better to use asyncio's
        'async def' syntax if it is available. Only async RTD functions are
        calculated in the background in Excel, non-RTD functions will be stopped
        if calculation is interrupted.
    threaded: bool
        Declares the function as safe for multi-threaded calculation. The
        function must be careful when accessing global objects. 
        Since python (at least CPython) is single-threaded there is
        no direct performance benefit from enabling this. However, if you make 
        frequent calls to C-based libraries like numpy or pandas you make
        be able to realise speed gains.
    volatile: bool
        Tells Excel to recalculate this function on every calc cycle: the same
        behaviour as the NOW() and INDIRECT() built-ins.  Due to the performance 
        hit this brings, it is rare that you will need to use this attribute.

    """

    arguments = locals()
    def decorate(fn):

        _is_async = is_async
        if inspect.iscoroutinefunction(fn) or inspect.isasyncgenfunction(fn):
            fn = async_wrapper(fn)
            _is_async = True

        descr = FuncDescription(fn)

        for arg, val in arguments.items():
            if not arg in ['fn', 'args', 'name', 'help']:
                descr.__dict__[arg] = val
        if name is not None:
            descr.name = name
        if help is not None:
            descr.help = help

        if args is not None:
            arg_names = [x.name.casefold() for x in descr.args]
            if type(args) is dict:
                for arg_name, arg_help in args.items():
                    try:
                        i = arg_names.index(arg_name.casefold())
                        descr.args[i].help = arg_help
                    except ValueError:
                        raise Exception(f"No parameter '{arg_name}' in function {fn.__name__}")
            else:
                for arg in args:
                    try:
                        i = arg_names.index(arg.name.casefold())
                        descr.args[i] = arg
                    except ValueError:
                        raise Exception(f"No parameter '{arg_name}' in function {fn.__name__}")
        
        descr.is_async = _is_async

        fn.__dict__[_META_TAG] = descr
        return fn

    return decorate if fn is None else decorate(fn)

_excel_application_com_obj = None

# TODO: Option to use win32com instead of comtypes?
def app():
    """
        Returns a handle to the Excel.Application object using the *comtypes* 
        library. The Excel.Application object is the root of Excel's COM
        interface and supports a wide range of operations. It is well 
        documented by Microsoft, see 
        https://docs.microsoft.com/en-us/visualstudio/vsto/excel-object-model-overview
        and 
        https://docs.microsoft.com/en-us/office/vba/api/excel.application(object).
        
        Many operations using the Application object will only work in 
        functions declared as **macro type**.

        Examples
        --------

        To get the name of the active worksheet:

        ::
            
            @func(macro=True)
            def sheetName():
                return xlo.app().ActiveSheet.Name

    """
    global _excel_application_com_obj
    if _excel_application_com_obj is None:
        import comtypes.client
        import comtypes
        import ctypes
        clsid = comtypes.GUID.from_progid("Excel.Application")
        obj = ctypes.POINTER(comtypes.IUnknown)(xloil_core.application())
        _excel_application_com_obj = comtypes.client._manage(obj, clsid, None)
    return _excel_application_com_obj
     


class EventsPaused():
    """
    A context manager which stops Excel events from firing whilst
    the context is in scope
    """
    def __enter__(self):
        event.pause()
        return self
    def __exit__(self, type, value, traceback):
        event.allow()

class _ModuleFinder(importlib.abc.MetaPathFinder):

    """
    Allows importing a module from a path specified in path_map
    without needing to add it to sys.paths - essentially a private
    set of import paths, indexed by module name
    """

    path_map = dict()

    def find_spec(self, fullname, path, target=None):
        path = self.path_map.get(fullname, None)
        if path is None:
            return None
        return importlib.util.spec_from_file_location(fullname, self.path_map[fullname])

    def find_module(self, fullname, path):
        return None


_module_finder = _ModuleFinder()
sys.meta_path.append(_module_finder)

## TODO: hook import and recurse looking for xloil funcs
## TODO: rename to maybe xl_import
def scan_module(module, workbook_name=None):
    """
        Parses a specified module to look for functions with with the xloil.func 
        decorator and register them. Does not search inside second level imports.

        The argument can be a module object, module name or path string. The module 
        is first imported if it has not already been loaded.
 
        Called by the xlOil C layer to import modules specified in the config.
    """

    with EventsPaused() as paused:

        if type(module) is str:
            mod_directory, filename = os.path.split(module)
            filename = filename.replace('.py', '')

            # avoid name collisions when loading workbook modules
            mod_name = filename if workbook_name is None else "xloil_wb_" + filename

            if len(mod_directory) > 0 or workbook_name is not None:
                _module_finder.path_map[mod_name] = module
   
            handle = importlib.import_module(mod_name)

            # Allows 'local' modules to know which workbook they link to
            if workbook_name is not None:
                handle._xloil_workbook = workbook_name
                handle._xloil_workbook_path = os.path.join(mod_directory, workbook_name)

        elif (inspect.ismodule(module) and hasattr(module, '__file__')) or module in sys.modules:
            # We can only reload modules with a __file__ attribute, e.g. not
            # xloil_core
            handle = importlib.reload(module)
        else:
            raise Exception(f"scan_module: could not process {str(module)}")

    
        # Look for functions with an xloil decorator (_META_TAG) and create
        # a function holder object for each of them
        xloil_funcs = inspect.getmembers(handle, 
            lambda obj: inspect.isfunction(obj) and hasattr(obj, _META_TAG))

        to_register = []
        for f_name, f in xloil_funcs:
            import traceback
            try:
                to_register.append(_get_meta(f).create_holder())
            except Exception as e:
                log(f"Register failed for {f_name}: {traceback.format_exc()}", level='error')

        if any(to_register):
            xloil_core.register_functions(handle, to_register)

        return handle
