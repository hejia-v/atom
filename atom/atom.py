#------------------------------------------------------------------------------
# Copyright (c) 2014, Nucleic
#
# Distributed under the terms of the BSD 3-Clause License.
#
# The full license is in the file LICENSE, distributed with this software.
#------------------------------------------------------------------------------
import six

from .catom import CAtom, _atom_meta_create_class


def __newobj__(cls, *args):
    """ A compatibility pickler function.

    This function is not part of the public Atom api.

    """
    return cls.__new__(cls, *args)


class AtomMeta(type):
    """ The metaclass for classes derived from Atom.

    This metaclass computes the atom member layout for the class so
    that the CAtom class can allocate exactly enough space for the
    instance data slots when it instantiates an object.

    All classes deriving from Atom are automatically slotted.

    """
    __new__ = _atom_meta_create_class


@six.add_metaclass(AtomMeta)
class Atom(CAtom):
    """ The base class for defining atom objects.

    Atom objects are special Python objects which never allocate an
    instance dictionary unless one is explicitly requested. The data
    storage for an atom instance is instead computed from the Member
    objects declared in the class body. Memory is reserved for these
    members with no over-allocation.

    This restriction make atom objects a bit less flexible than normal
    Python objects, but they are 3x - 10x more memory efficient than
    normal objects, and are 10% - 20%  faster on attribute access.

    """
    def __reduce_ex__(self, proto):
        """ An implementation of the reduce protocol.

        This method creates a reduction tuple for Atom instances. This
        method should not be overridden by subclasses unless the author
        fully understands the rammifications.

        """
        args = (type(self),) + self.__getnewargs__()
        return (__newobj__, args, self.__getstate__())

    def __getnewargs__(self):
        """ Get the argument tuple to pass to __new__ on unpickling.

        See the Python.org docs for more information.

        """
        return ()

    def __getstate__(self):
        """ The base implementation of the pickle getstate protocol.

        This base class implementation handles the generic case where
        the object and all of its state are pickable. Subclasses which
        require custom behavior should reimplement this method.

        """
        state = {}
        for key in self.get_members():
            state[key] = getattr(self, key)
        return state

    def __setstate__(self, state):
        """ The base implementation of the pickle setstate protocol.

        This base class implementation handle the generic case of
        restoring an object using the state generated by the base
        class __getstate__ method. Subclasses which require custom
        behavior should reimplement this method.

        """
        for key, value in state.iteritems():
            setattr(self, key, value)
