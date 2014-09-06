/*-----------------------------------------------------------------------------
| Copyright (c) 2014, Nucleic Development Team.
|
| Distributed under the terms of the Modified BSD License.
|
| The full license is in the file COPYING.txt, distributed with this software.
|----------------------------------------------------------------------------*/
#include "atom.h"
#include "member.h"
#include "py23compat.h"
#include "utils.h"

#include <algorithm>

#define atom_cast( o ) reinterpret_cast<Atom*>( o )
#define member_cast( o ) reinterpret_cast<Member*>( o )
#define signal_cast( o ) reinterpret_cast<Signal*>( o )
#define pyobject_cast( o ) reinterpret_cast<PyObject*>( o )


namespace atom
{

namespace
{

typedef Atom::CSVector CSVector;


PyObject* registry;


struct CmpLess
{
	template <typename T>
	bool operator()( T& lhs, Signal* rhs )
	{
		return lhs.first < pyobject_cast( rhs );
	}

	template <typename T>
	bool operator()( Signal* lhs, T& rhs )
	{
		return pyobject_cast( lhs ) < rhs.first;
	}
};


struct CmpEqual
{
	template <typename T>
	bool operator()( T& lhs, Signal* rhs )
	{
		return lhs.first == pyobject_cast( rhs );
	}

	template <typename T>
	bool operator()( Signal* lhs, T& rhs )
	{
		return pyobject_cast( lhs ) == rhs.first;
	}
};


inline CSVector::iterator lowerBound( CSVector* cbsets, Signal* sig )
{
	return std::lower_bound( cbsets->begin(), cbsets->end(), sig, CmpLess() );
}


inline CSVector::iterator binaryFind( CSVector* cbsets, Signal* sig )
{
	CSVector::iterator it = lowerBound( cbsets, sig );
	if( it != cbsets->end() && CmpEqual()( *it, sig ) )
	{
		return it;
	}
	return cbsets->end();
}


Py_ssize_t getsizeof( CSVector* cbsets )
{
	Py_ssize_t extras = 0;
	typedef CSVector::iterator iter_t;
	for( iter_t it = cbsets->begin(), end = cbsets->end(); it != end; ++it )
	{
		if( it->second.extras() )
		{
			Py_ssize_t size = sys_getsizeof( it->second.extras() );
			if( size < 0 && PyErr_Occurred() )
			{
				return -1;
			}
			extras += size;
		}
	}
	Py_ssize_t vec = static_cast<Py_ssize_t>( sizeof( CSVector ) );
	Py_ssize_t val = static_cast<Py_ssize_t>( sizeof( CSVector::value_type ) );
	Py_ssize_t cap = static_cast<Py_ssize_t>( cbsets->capacity() );
	return vec + cap * val + extras;
}


PyObject* Atom_new( PyTypeObject* type, PyObject* args, PyObject* kwargs )
{
	cppy::ptr members( Atom::LookupMembers( type ) );
	if( !members )
	{
		return 0;
	}
	Py_ssize_t size = PyDict_Size( members.get() );
	cppy::ptr self( type->tp_alloc( type, size ) );
	if( !self )
	{
		return 0;
	}
	Atom* atom = atom_cast( self.get() );
	atom->m_members = members.release();
	return self.release();
}


int Atom_init( PyObject* self, PyObject* args, PyObject* kwargs )
{
	if( PyTuple_GET_SIZE( args ) > 0 )
	{
		cppy::type_error( "__init__() takes no positional arguments" );
		return -1;
	}
	if( kwargs )
	{
		PyObject* key;
		PyObject* value;
		Py_ssize_t pos = 0;
		while( PyDict_Next( kwargs, &pos, &key, &value ) )
		{
			if( PyObject_SetAttr( self, key, value ) < 0 )
			{
				return -1;
			}
		}
	}
	return 0;
}


int Atom_clear( Atom* self )
{
	if( self->m_cbsets )
	{
		CSVector temp; // safe clear
		self->m_cbsets->swap( temp );
	}
	for( Py_ssize_t i = 0, n = Py_SIZE( self ); i < n; ++i )
	{
		Py_CLEAR( self->m_values[ i ] );
	}
	Py_CLEAR( self->m_members );
	return 0;
}


int Atom_traverse( Atom* self, visitproc visit, void* arg )
{
	if( self->m_cbsets )
	{
		typedef CSVector::iterator iter_t;
		iter_t end = self->m_cbsets->end();
		for( iter_t it = self->m_cbsets->begin(); it != end; ++it )
		{
			Py_VISIT( it->first.get() );
			Py_VISIT( it->second.single() );
			Py_VISIT( it->second.extras() );
		}
	}
	for( Py_ssize_t i = 0, n = Py_SIZE( self ); i < n; ++i )
	{
		Py_VISIT( self->m_values[ i ] );
	}
	Py_VISIT( self->m_members );
	return 0;
}


void Atom_dealloc( Atom* self )
{
	PyObject_GC_UnTrack( self );
	if( self->m_weaklist )
	{
		PyObject_ClearWeakRefs( pyobject_cast( self ) );
	}
	Atom_clear( self );
	delete self->m_cbsets;
	self->ob_type->tp_free( pyobject_cast( self ) );
}


PyObject* Atom_getattro( Atom* self, PyObject* name )
{
	// This is not *strictly* a known-safe cast. While effort is made
	// ensure that the user does not have access to the member registry
	// and hence cannot modify the dict, the GC module will still allow
	// the user to dig into it and add a non-member. My stance is that
	// if they do that, they deserve the segfault. I don't want to pay
	// the extra type checking cost just to protect against a motivated
	// attacker. You can always crash the interpreted with ctypes anyway.
	Member* member = member_cast( PyDict_GetItem( self->m_members, name ) );
	if( member )
	{
		cppy::ptr valptr( self->m_values[ member->index() ], true );
		if( valptr )
		{
			return valptr.release();
		}
		valptr = member->defaultValue( pyobject_cast( self ), name );
		if( !valptr )
		{
			return 0;
		}
		self->m_values[ member->index() ] = cppy::incref( valptr.get() );
		return valptr.release();
	}
	return PyObject_GenericGetAttr( pyobject_cast( self ), name );
}


int Atom_setattro( Atom* self, PyObject* name, PyObject* value )
{
	// This is not *strictly* a known-safe cast. While effort is made
	// ensure that the user does not have access to the member registry
	// and hence cannot modify the dict, the GC module will still allow
	// the user to dig into it and add a non-member. My stance is that
	// if they do that, they deserve the segfault. I don't want to pay
	// the extra type checking cost just to protect against a motivated
	// attacker. You can always crash the interpreted with ctypes anyway.
	Member* member = member_cast( PyDict_GetItem( self->m_members, name ) );
	if( member )
	{
		if( !value )
		{
			cppy::clear( &self->m_values[ member->index() ] );
			return 0;
		}
		cppy::ptr valptr( member->validate( pyobject_cast( self ), name, value ) );
		if( !valptr )
		{
			return -1;
		}
		cppy::replace( &self->m_values[ member->index() ], valptr.get() );
		return 0;
	}
	return PyObject_GenericSetAttr( pyobject_cast( self ), name, value );
}


PyObject* Atom_get_member( Atom* self, PyObject* name )
{
	if( !Py23Str_Check( name ) )
	{
		return cppy::type_error( name, "str" );
	}
	PyObject* pyo = PyDict_GetItem( self->m_members, name );
	return cppy::incref( pyo ? pyo : Py_None );
}


PyObject* Atom_get_members( Atom* self, PyObject* args )
{
	return PyDict_Copy( self->m_members );
}


PyObject* Atom_connect( Atom* self, PyObject* args )
{
	PyObject* sig;
	PyObject* callback;
	if( !PyArg_ParseTuple( args, "OO", &sig, &callback ) )
	{
		return 0;
	}
	if( !Signal::TypeCheck( sig ) )
	{
		return cppy::type_error( sig, "Signal" );
	}
	if( !PyCallable_Check( callback ) )
	{
		return cppy::type_error( callback, "callable" );
	}
	// TODO support weak methods
	self->connect( signal_cast( sig ), callback );
	return cppy::incref( Py_None );
}


PyObject* Atom_disconnect( Atom* self, PyObject* args )
{
	PyObject* sig = 0;
	PyObject* callback = 0;
	if( !PyArg_ParseTuple( args, "|OO", &sig, &callback ) )
	{
		return 0;
	}
	if( sig && !Signal::TypeCheck( sig ) )
	{
		return cppy::type_error( sig, "Signal" );
	}
	if( callback && !PyCallable_Check( callback ) )
	{
		return cppy::type_error( callback, "callable" );
	}
	if( !sig )
	{
		self->disconnect();
	}
	else if( !callback )
	{
		self->disconnect( signal_cast( sig ) );
	}
	else
	{
		// TODO support weak methods
		self->disconnect( signal_cast( sig ), callback );
	}
	return cppy::incref( Py_None );
}


PyObject* Atom_emit( Atom* self, PyObject* args, PyObject* kwargs )
{
	Py_ssize_t arg_count = PyTuple_GET_SIZE( args );
	if( arg_count == 0 )
	{
		return cppy::type_error( "emit() takes at least 1 argument (0 given)" );
	}
	PyObject* sig = PyTuple_GET_ITEM( args, 0 );
	if( !Signal::TypeCheck( sig ) )
	{
		return cppy::type_error( sig, "Signal" );
	}
	// TODO can this be made faster?
	cppy::ptr rest( PyTuple_GetSlice( args, 1, arg_count ) );
	// TODO push to a sender stack
	self->emit( signal_cast( sig ), rest.get(), kwargs );
	// TODO pop from a sender stack
	return cppy::incref( Py_None );
}


PyObject* Atom_sizeof( Atom* self, PyObject* args )
{
	Py_ssize_t basic = self->ob_type->tp_basicsize;
	Py_ssize_t items = Py_SIZE( self ) * sizeof( PyObject* );
	Py_ssize_t cbsets = self->m_cbsets ? getsizeof( self->m_cbsets ) : 0;
	if( cbsets < 0 && PyErr_Occurred() )
	{
		return 0;
	}
	return Py23Int_FromSsize_t( basic + items + cbsets );
}


PyMethodDef Atom_methods[] = {
	{ "get_member",
	  ( PyCFunction )Atom_get_member,
	  METH_O,
	  "get_member(name) get the named member for the object or None" },
	{ "get_members",
	  ( PyCFunction )Atom_get_members,
	  METH_NOARGS,
	  "get_members() get all of the members for the object as a dict" },
	{ "connect",
	  ( PyCFunction )Atom_connect,
	  METH_VARARGS,
	  "connect(signal, callback) connect a signal to a callback" },
	{ "disconnect",
	  ( PyCFunction )Atom_disconnect,
	  METH_VARARGS,
	  "disconnect([signal[, callback]) disconnect a signal from a callback" },
	{ "emit",
	  ( PyCFunction )Atom_emit,
	  METH_VARARGS | METH_KEYWORDS,
	  "emit(signal, *args, **kwargs) emit a signal with the given arguments" },
	{ "__sizeof__",
	  ( PyCFunction )Atom_sizeof,
	  METH_NOARGS,
	  "__sizeof__() -> size of object in memory, in bytes" },
	{ 0 } // sentinel
};

} // namespace


PyTypeObject Atom::TypeObject = {
	PyVarObject_HEAD_INIT( &PyType_Type, 0 )
	"atom.catom.CAtom",
	sizeof( Atom ) - sizeof( PyObject* ),
	sizeof( PyObject* ),
	( destructor )Atom_dealloc,          /* tp_dealloc */
	( printfunc )0,                      /* tp_print */
	( getattrfunc )0,                    /* tp_getattr */
	( setattrfunc )0,                    /* tp_setattr */
	( cmpfunc )0,                        /* tp_compare */
	( reprfunc )0,                       /* tp_repr */
	( PyNumberMethods* )0,               /* tp_as_number */
	( PySequenceMethods* )0,             /* tp_as_sequence */
	( PyMappingMethods* )0,              /* tp_as_mapping */
	( hashfunc )0,                       /* tp_hash */
	( ternaryfunc )0,                    /* tp_call */
	( reprfunc )0,                       /* tp_str */
	( getattrofunc )Atom_getattro,       /* tp_getattro */
	( setattrofunc )Atom_setattro,       /* tp_setattro */
	( PyBufferProcs* )0,                 /* tp_as_buffer */
	Py_TPFLAGS_DEFAULT
	| Py_TPFLAGS_BASETYPE
	| Py_TPFLAGS_HAVE_GC
	| Py_TPFLAGS_HAVE_VERSION_TAG,       /* tp_flags */
	0,                                   /* Documentation string */
	( traverseproc )Atom_traverse,       /* tp_traverse */
	( inquiry )Atom_clear,               /* tp_clear */
	( richcmpfunc )0,                    /* tp_richcompare */
	offsetof(Atom, m_weaklist),          /* tp_weaklistoffset */
	( getiterfunc )0,                    /* tp_iter */
	( iternextfunc )0,                   /* tp_iternext */
	( struct PyMethodDef* )Atom_methods, /* tp_methods */
	( struct PyMemberDef* )0,            /* tp_members */
	0,                                   /* tp_getset */
	0,                                   /* tp_base */
	0,                                   /* tp_dict */
	( descrgetfunc )0,                   /* tp_descr_get */
	( descrsetfunc )0,                   /* tp_descr_set */
	0,                                   /* tp_dictoffset */
	( initproc )Atom_init,               /* tp_init */
	( allocfunc )PyType_GenericAlloc,    /* tp_alloc */
	( newfunc )Atom_new,                 /* tp_new */
	( freefunc )PyObject_GC_Del,         /* tp_free */
	( inquiry )0,                        /* tp_is_gc */
	0,                                   /* tp_bases */
	0,                                   /* tp_mro */
	0,                                   /* tp_cache */
	0,                                   /* tp_subclasses */
	0,                                   /* tp_weaklist */
	( destructor )0                      /* tp_del */
};


bool Atom::Ready()
{
	if( !( registry = PyDict_New() ) )
	{
		return false;
	}
	return PyType_Ready( &TypeObject ) == 0;
}


bool Atom::RegisterMembers( PyTypeObject* type, PyObject* members )
{
	return PyDict_SetItem( registry, pyobject_cast( type ), members ) == 0;
}


PyObject* Atom::LookupMembers( PyTypeObject* type )
{
	PyObject* members = PyDict_GetItem( registry, pyobject_cast( type ) );
	if( members )
	{
		return cppy::incref( members );
	}
	return cppy::type_error( "type has no registered members" );
}


void Atom::connect( Signal* sig, PyObject* callback )
{
	if( !m_cbsets )
	{
		m_cbsets = new CSVector();
	}
	cppy::ptr pyptr( pyobject_cast( sig ), true );
	CSVector::iterator it = lowerBound( m_cbsets, sig );
	if( it == m_cbsets->end() || it->first != pyptr )
	{
		CallbackSet cbset( callback );
		m_cbsets->insert( it, CSPair( pyptr, cbset ) );
	}
	else
	{
		it->second.add( callback );
	}
}


void Atom::disconnect()
{
	if( m_cbsets )
	{
		CSVector temp;  // safe clear
		m_cbsets->swap( temp );
	}
}


void Atom::disconnect( Signal* sig )
{
	if( m_cbsets )
	{
		CSVector::iterator it = binaryFind( m_cbsets, sig );
		if( it != m_cbsets->end() )
		{
			m_cbsets->erase( it );
		}
	}
}


void Atom::disconnect( Signal* sig, PyObject* callback )
{
	if( m_cbsets )
	{
		CSVector::iterator it = binaryFind( m_cbsets, sig );
		if( it != m_cbsets->end() )
		{
			it->second.remove( callback );
		}
	}
}


void Atom::emit( Signal* sig, PyObject* args, PyObject* kwargs )
{
	if( m_cbsets )
	{
		CSVector::iterator it = binaryFind( m_cbsets, sig );
		if( it != m_cbsets->end() )
		{
			it->second.dispatch( args, kwargs );
		}
	}
}

} // namespace atom
