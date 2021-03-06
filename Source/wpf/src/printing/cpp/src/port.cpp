/*++

    Copyright (C) 2002 - 2003 Microsoft Corporation
    All rights reserved.

    Module Name:
        Port.cpp

    Abstract:

    Author:
        Khaled Sedky (khaleds) 10-January-2003


    Revision History:
--*/
#include "win32inc.hpp"

using namespace System;
using namespace System::IO;
using namespace System::Collections;
using namespace System::Reflection;
using namespace System::Runtime::InteropServices;
using namespace System::Xml;
using namespace System::Xml::XPath;
using namespace System::Collections::Specialized;

#ifndef  __PRINTSYSTEMINTEROPINC_HPP__
#include <PrintSystemInteropInc.hpp>
#endif

#ifndef  __PRITNSYSTEMINC_HPP__
#include <PrintSystemInc.hpp>
#endif

#ifndef  __PRINTSYSTEMPATHRESOLVER_HPP__
#include <PrintSystemPathResolver.hpp>
#endif

using namespace System::Printing;

#ifndef  __PRINTSYSTEMATTRIBUTEVALUEFACTORY_HPP__
#include <PrintSystemAttributeValueFactory.hpp>
#endif

#ifndef  __PRINTSYSTEMOBJECTFACTORY_HPP__
#include <PrintSystemObjectFactory.hpp>
#endif

using namespace System::Printing::Activation;

PrintPort::
PrintPort(
    String^ portName
    )
{
    Name = portName;
    accessVerifier = gcnew PrintSystemDispatcherObject();
}

void
PrintPort::
InternalDispose(
    bool disposing
    )
{
    if(!this->IsDisposed)
    {
        System::Threading::Monitor::Enter(this);
        {
            __try
            {
                PrintSystemObject::InternalDispose(disposing);
            }
            __finally
            {
                this->IsDisposed = true;
                System::Threading::Monitor::Exit(this);
            }
        }
    }
}

void
PrintPort::
Commit(
    void
    )
{
    VerifyAccess();
}

void
PrintPort::
Refresh(
    void
    )
{
    VerifyAccess();
}

PrintPropertyDictionary^
PrintPort::
get_InternalPropertiesCollection(
    String^ attributeName
    )
{
    return nullptr;
}

/*++
    Function Name:
        RegisterAttributesNamesTypes

    Description:
        The way the APIs work is that every compile time
        property is linked internally to a named property.
        The named property is an attribute / value pair.
        This pair has a generic type inheriting form
        PrintProperty and the specific type is
        determined by the type of the compile time property.
        By registering the named property and giving it a type,
        later on it is pretty easy to determine which specific
        type should be assigned to this named property in the
        property collection.

    Parameters:
        None

    Return Value
        None
--*/
void
PrintPort::
RegisterAttributesNamesTypes(
    void
    )
{
    //
    // Register the attributes of the base class first
    //
    PrintSystemObject::RegisterAttributesNamesTypes(PrintPort::attributeNameTypes);
}

/*++
    Function Name:
        CreateAttributeNoValue

    Description:
        When the internal collection of proeprties for an object is
        created, the way individual properties are added to that
        collection is through using a factory. The reason for using a
        factory, is that every object is delegated adding its properties
        to its internal collection. Reason for that is that the object
        best knows it properties and their types.

    Parameters:
        String: The name of the property

    Return Value
        PrintProperty:  The property created as an
                        Attribute / Value pair
--*/
PrintProperty^
PrintPort::
CreateAttributeNoValue(
    String^ attributeName
    )
{
    Type^ type = (Type^)PrintPort::attributeNameTypes[attributeName];

    return PrintPropertyFactory::Value->Create(type,attributeName);
}


/*++
    Function Name:
        CreateAttributeValue

    Description:
        When the internal collection of proeprties for an object is
        created, the way individual properties are added to that
        collection is through using a factory. The reason for using a
        factory, is that every object is delegated adding its properties
        to its internal collection. Reason for that is that the object
        best knows it properties and their types.

    Parameters:
        String: The name of the property
        Object: The value of the property

    Return Value
        PrintProperty:  The property created as an
                        Attribute / Value pair
--*/
PrintProperty^
PrintPort::
CreateAttributeValue(
    String^ attributeName,
    Object^ attributeValue
    )
{
    Type^ type = (Type^)PrintPort::attributeNameTypes[attributeName];

    return PrintPropertyFactory::Value->Create(type,attributeName,attributeValue);
}


/*++
    Function Name:
        CreateAttributeNoValueLinked

    Description:
        When the internal collection of proeprties for an object is
        created, the way individual properties are added to that
        collection is through using a factory. The reason for using a
        factory, is that every object is delegated adding its properties
        to its internal collection. Reason for that is that the object
        best knows it properties and their types.

    Parameters:
        String:             The name of the property
        MulticastDelegate:  The delegate linking the named property to
                            a compile time property.

    Return Value
        PrintProperty:  The property created as an
                        Attribute / Value pair
--*/
PrintProperty^
PrintPort::
CreateAttributeNoValueLinked(
    String^             attributeName,
    MulticastDelegate^  delegate
    )
{
    Type^ type = (Type^)PrintPort::attributeNameTypes[attributeName];

    return PrintPropertyFactory::Value->Create(type,attributeName,delegate);
}


/*++
    Function Name:
        CreateAttributeValueLinked

    Description:
        When the internal collection of proeprties for an object is
        created, the way individual properties are added to that
        collection is through using a factory. The reason for using a
        factory, is that every object is delegated adding its properties
        to its internal collection. Reason for that is that the object
        best knows it properties and their types.

    Parameters:
        String:             The name of the property
        Object:             The value of the property
        MulticastDelegate:  The delegate linking the named property to
                            a compile time property.

    Return Value
        PrintProperty:  The property created as an
                        Attribute / Value pair
--*/
PrintProperty^
PrintPort::
CreateAttributeValueLinked(
    String^             attributeName,
    Object^             attributeValue,
    MulticastDelegate^  delegate
    )
{
    Type^ type = (Type^)PrintPort::attributeNameTypes[attributeName];

    return PrintPropertyFactory::Value->Create(type,attributeName,attributeValue,delegate);
}

void
PrintPort::
VerifyAccess(
    void
    )
{
    if(accessVerifier==nullptr)
    {
        accessVerifier = gcnew PrintSystemDispatcherObject();
    }

    accessVerifier->VerifyThreadLocality();
}

