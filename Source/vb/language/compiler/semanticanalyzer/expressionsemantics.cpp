//-------------------------------------------------------------------------------------------------
//
//  Copyright (c) Microsoft Corporation.  All rights reserved.
//
//  Implementation of VB expression semantic analysis. 
//
//-------------------------------------------------------------------------------------------------

#include "StdAfx.h"

#include "ExpressionTreeSemantics.h"
#include "ILTreeETGenerator.h"

#define RETURN_IF_NOT_NULL(type, expr)              \
    {                                               \
        type __return_if_not_null_tmp__ = (expr);   \
        if (__return_if_not_null_tmp__)             \
        {                                           \
            return __return_if_not_null_tmp__;      \
        }                                           \
    }

#define LAMBDA_PARAMETER_PREFIX L"$"
#define CONTINUE_LOOP_IF_FALSE(condition) if (!(condition)) { continue; }


ConstantValue Semantics::InterpretConstantExpression(
    ParseTree::Expression *Input,
    SourceFile *File,
    Scope *Lookup,
    BCSYM *TargetType,
    NorlsAllocator *TreesStorage,
    ErrorTable *Errors,
    bool ConditionalCompilationExpression,
    Compiler *TheCompiler,
    CompilerHost *TheCompilerHost,
    bool *ResultIsBad,
    bool IsSyntheticExpression,
    bool disableCaching,
    Declaration *ContextOfSymbolUsage)
{
    if (TargetType)
    {
        TargetType = TargetType->DigThroughAlias();
    }

    if (File == NULL && Lookup)
    {
        File = Lookup->GetSourceFile();
    }

    Semantics Analyzer(TreesStorage, Errors, TheCompiler, TheCompilerHost, File, NULL, false);
    if (disableCaching)
    {
        Analyzer.m_PermitDeclarationCaching = false;
    }

    return
        Analyzer.InterpretConstantExpression(
            Input,
            Lookup,
            TargetType,
            ConditionalCompilationExpression,
            ResultIsBad,
            ContextOfSymbolUsage,
            IsSyntheticExpression);
}


Symbol *
Semantics::AttemptInterpretLocalReference
(
    Location & location,
    _In_z_ Identifier * Name,
    NameFlags NameLookupFlags,
    ExpressionFlags ExprFlags,
    int GenericTypeArity,
    ILTree::Expression ** ppResult, //if we detect an error then we will place a bad expression here
    bool CheckUseOfLocalBeforeDeclaration /* = true */
)
{
    bool badTemp = false;
    GenericBinding * pGenericBindingContext = NULL;

    Symbol * pDecl =
        InterpretName
        (
            Name,
            m_Lookup,
            NULL,
            NameLookupFlags |
                NameSearchLocalsOnly |
                NameSearchIgnoreImports ,
            ContainingClass(),
            location,
            badTemp,
            &pGenericBindingContext,
            GenericTypeArity
        );


    if (pDecl && ! badTemp)
    {
        Location * pDeclLocation = pDecl->GetLocation();

        if (CheckUseOfLocalBeforeDeclaration  &&
            pDeclLocation && Location::CompareStartPoints(pDeclLocation, &location) > 0)
        {
            if (m_Errors && ! m_Errors->HasThisErrorWithLocation(ERRID_UseOfLocalBeforeDeclaration1, location))
            {
                ReportSemanticError(ERRID_UseOfLocalBeforeDeclaration1, location, Name);
            }

            if (ppResult)
            {
                *ppResult = AllocateBadExpression(location);
            }

            return NULL;
        }
        else
        {
            return pDecl;
        }
    }
    else
    {
        return NULL;
    }
}



ILTree::Expression *
Semantics::InterpretNameExpression
(
    ParseTree::Expression *Input,
    ExpressionFlags Flags,
    int GenericTypeArity,
    Location *GenericTypeArgsLoc
)
{
    ILTree::Expression * Result = NULL;

    if (m_Lookup == NULL)
    {
        // If there is no scope in which to look up names, the compiler is
        // attempting to process simple constant expressions, and needs a cue
        // that this is not a simple constant expression. Actually generating
        // an error message would be incorrect here.
        //
        // 

        return AllocateBadExpression(Input->TextSpan);
    }

    if (Input->AsName()->Name.IsBad)
    {
        return AllocateBadExpression(Input->TextSpan);
    }

    bool NameIsBad = false;
    Identifier *Name = Input->AsName()->Name.Name;

    NameFlags NameLookupFlags = NameNoFlags;

    if (HasFlag(Flags, ExprTypeReferenceOnly))
    {
        SetFlag(NameLookupFlags, NameSearchTypeReferenceOnly);
    }
    if (m_EvaluatingConditionalCompilationConstants)
    {
        SetFlag(NameLookupFlags, NameSearchConditionalCompilation);
    }
    if (HasFlag(Flags, ExprLeadingQualifiedName))
    {
        SetFlag(NameLookupFlags, NameSearchLeadingQualifiedName);
        SetFlag(NameLookupFlags, NameSearchFirstQualifiedName);
    }

    GenericBinding *GenericBindingContext = NULL;

    Symbol * NameBinding = NULL;

    if (m_UseQueryNameLookup)
    {
        Result = AttemptQueryNameLookup(Input->TextSpan,Name, Input->AsName()->Name.TypeCharacter,
                                                            NameLookupFlags, Flags, GenericTypeArity, &NameBinding);
    }

    if (! Result)
    {
        if (!NameBinding)
        {

            // Microsoft:
            // To fix the "inaccessible name binding" issues, InterpretName may report errors
            // for inaccessible types. Use a temporary error table to keep these errors, and throw them
            // out if we don't need them.

            TemporaryErrorTable backup_error_table(m_Compiler, &m_Errors);
            backup_error_table.SuppressMergeOnRestore();
            if (m_Errors)
            {
                backup_error_table.AddTemporaryErrorTable(new ErrorTable(*m_Errors));
                m_Errors = backup_error_table.NewErrorTable(0);
            }

            bool ignored = false;

            NameBinding =
                InterpretName
                (
                    Name,
                    m_Lookup,
                    NULL,   // No Type parameter lookup
                    NameLookupFlags,
                    ContainingClass(), 
                    Input->TextSpan,
                    NameIsBad,
                    &GenericBindingContext,
                    GenericTypeArity,
                    &ignored
                );

            // Might yield NameIsBad=false / NameBinding==NULL, e.g. if it was asked to lookup a name that doesn't exist
            // Might yield NameIsBad=false / NameBinding!=NULL, e.g. the standard success scenario
            // Might yield NameIsBad=true  / NameBinding!=NULL, e.g. if it returns GenericBadNamedRoot,
            //                                                       or a reference to an inaccessible type
            // Might yield NameIsBad=true  / NameBinding==NULL, e.g. ambiguous name lookup between two modules

            // In the following code we have to worry about "implicitly declared variables".
            // "For x = 1 to 5". 
            //    or
            // "x=1"
            //
            // These are the rules when we create implicit variable 
            // (a loop control variable or a local variable), assuming implicit declarations are allowed (m_CreateImplicitDeclarations):
            //
            // (IC1) If x does not bind to anything. In this case we actually don't have to do anything special because NameIsBad=false and NameBinding==NULL.
            //
            // (IC2) If x refers to a type, but type references are not allowed in the context.
            //       
            // (IC3) If x refers to something inaccessible.
            //
            //
            // At the same time the following code has to work with normal bindings, e.g. "x()".
            // Here it's straightforward: report the appropriate error if it can't bind or if it is ambiguous;
            // but if it can bind unambiguously, then bind.
            // There are a complicated variety of error messages that we give for the various failed-to-bind scenarios.
            // Dev10#487250.



            // If we won't create a control variable, then keep the errors.
            // Note that (IC1) didn't report errors into the error table, returned NameIsBad=false and
            // NameBinding==NULL.

            // Dev10 #626191 Figure out if we should ignore result of the search in favor of an implicit variable declaration
            bool forceImplicitVariable = false;

            if (
                NameBinding!=NULL && 
                m_CreateImplicitDeclarations &&
                !HasFlag(Flags, ExprSuppressImplicitVariableDeclaration) &&
                !IsObsoleteStandaloneExpressionKeyword(Name)
                )
            {
                // Dev10 #672937 
                // Prior to the fix for [Dev10 #626191], types were always ignored 
                // when option infer is on, and we set the ExprInferLoopControlVariableExplicit
                // flag (set in InterpretForStatement). 
                // Let's continue taking only this shortcut for types.
                if (
                    NameBinding->IsType() &&
                    OptionInferOn() &&
                    HasFlag(Flags, ExprInferLoopControlVariableExplicit))
                {
                    forceImplicitVariable = true;
                }
                else if ( NameIsBad && !IsBad(ChaseThroughAlias(NameBinding))) // Dev10 #672890 Don't try to check accesibility of a bad symbol, the check will always report an error.
                {
                    // Dev10 #626191 check if result is accessible
                    bool isBadDueToAcessibility = false;
                    // Don't want to pick up any errors from the following Accessibility check, they are already reported.
                    ErrorTable * current = m_Errors;
                    
                    if (m_Errors)
                    {
                        ErrorTable * ignore  = new ErrorTable(*backup_error_table.OldErrorTable());
                        backup_error_table.AddTemporaryErrorTable(ignore);
                        m_Errors = ignore;
                    }
                    
                    Semantics::CheckAccessibility
                        (
                            NameBinding,
                            GenericBindingContext,
                            Input->TextSpan,
                            NameLookupFlags,
                            ContainingClass(),
                            isBadDueToAcessibility
                        );            

                    m_Errors = current;

                    if (isBadDueToAcessibility)
                    {
                        // We are bound to something that is inaccessible
                        forceImplicitVariable = true;
                    }
                }
            }

            if (forceImplicitVariable)
            {
                // If we should create an implicit variable, make it look like we haven't found anything 
                // and the rest of the function will do the right thing
                NameIsBad = false;
                NameBinding = NULL;
            }
            else if (m_Errors )
            {
                backup_error_table.EnableMergeOnRestore(0);
            }

            backup_error_table.Restore();
        }

        if (NameIsBad )
        {
            return AllocateBadExpression(Input->TextSpan);
        }

        // A recursive call within a function needs to bind to the function,
        // not to the result variable. This needs to occur during name
        // lookup, and not be patched up later, because the function might
        // be overloaded.

        if (HasFlag(Flags, ExprIsExplicitCallTarget) &&
            NameBinding &&
            NameBinding->IsVariable() &&
            NameBinding->PVariable()->IsFunctionResultLocal() &&
            m_Procedure)
        {
            // It isn't valid to just set the name binding to the containing
            // procedure, because the containing procedure might be
            // overloaded. Going through the name lookup mechanism again
            // takes care of this.

            NameBinding =
                InterpretName(
                    Name,
                    GetEnclosingNonLocalScope(),
                    NULL,   // No Type parameter lookup
                    NameLookupFlags,
                    ContainingClass(),
                    Input->TextSpan,
                    NameIsBad,
                    &GenericBindingContext,
                    GenericTypeArity);

            if (NameIsBad)
            {
                return AllocateBadExpression(Input->TextSpan);
            }
        }

        if (NameBinding==NULL)
        {
            VSASSERT(!NameIsBad, "how can an unfound name be a bad name?");

            if (m_EvaluatingConditionalCompilationConstants)
            {
                // Undeclared conditional compilation constants have
                // an implied value of Nothing.

                return AllocateExpression(SX_NOTHING, GetFXSymbolProvider()->GetObjectType(), Input->TextSpan);
            }
            else if (m_CreateImplicitDeclarations &&
                     !HasFlag(Flags, ExprSuppressImplicitVariableDeclaration) &&
                     !IsObsoleteStandaloneExpressionKeyword(Name))
            {
                Location* locToUse = NULL;

                // DevDiv #699
                // Microsoft: if option infer is on, we are creating an explicitly scoped variable,
                // so get the location. If option infer is off, this will create an implicit
                // variable, which requires no location.

                if(m_IsGeneratingXML || (OptionInferOn() && HasFlag(Flags, ExprInferLoopControlVariableExplicit)) )
                {
                    locToUse = &Input->TextSpan;
                }

                NameBinding =
                    CreateImplicitDeclaration(
                        Input->AsName()->Name.Name,
                        Input->AsName()->Name.TypeCharacter,
                        locToUse,
                        Flags);

                Assume(!NameBinding || NameBinding->IsNamedRoot(), L"How can an implicit variable not be a named root?");
                // 


                if (NameBinding)
                {
                    VSASSERT( (NameBinding->PNamedRoot()->GetParent() && NameBinding->PNamedRoot()->GetParent()->IsProc()) ||
                              (m_StatementLambdaInterpreter),
                              "How can there be implicit declaration anywhere else besides a proc or lambda body ?");

                    if (m_Procedure && IsGeneric(m_Procedure))
                    {
                        // The reference is to a declaration within a generic procedure.
                        GenericBindingContext = SynthesizeOpenGenericBinding(m_Procedure, m_SymbolCreator);
                    }
                    else
                    {
                        // The reference is not to a declaration within a generic procedure.

                        if (ContainingClass())
                        {
                            GenericBindingContext =
                                IsGenericOrHasGenericParent(ContainingClass()) ?
                                    SynthesizeOpenGenericBinding(ContainingClass(), m_SymbolCreator) :
                                    NULL;
                        }
                    }
                }
            }
            else if (HasFlag(Flags, ExprTreatQualifiedNamesEnMasse))
            {
                // When evaluating expressions under the debugger, a qualified
                // name of the form "a.b" might be available even if "a" is not.
                // Give the caller a shot at figuring this out.

                return
                    AllocateExpression(
                        SX_NAME_NOT_FOUND,
                        TypeHelpers::GetVoidType(),
                        Input->TextSpan);
            }
            else
            {
                VSASSERT(NameBinding == NULL && NameIsBad == false, "Unexpected state during name binding!!!");

                // Try looking it up as if it were an Xml prefix to report better error
                if (m_ReportErrors && InterpretXmlPrefix(NULL, Name, NameNoFlags, m_SourceFile))
                {
                    ReportSemanticError(
                        ERRID_XmlPrefixNotExpression,
                        Input->TextSpan,
                        Input);

                    return AllocateBadExpression(Input->TextSpan);
                }

                // Now try with "any" i.e. -1 arity to report better errors
                //
                if (m_ReportErrors && GenericTypeArity != -1)
                {

                    GenericBinding *TempGenericBindingContext = NULL;

                    m_ReportErrors = false;

                    //This case should not be an
                    Declaration *TempNameBinding =
                        EnsureNamedRoot
                        (
                            InterpretName
                            (
                                Name,
                                m_Lookup,
                                NULL,   // No Type parameter lookup
                                NameLookupFlags | NameSearchIgnoreExtensionMethods ,
                                ContainingClass(), 
                                Input->TextSpan,
                                NameIsBad,
                                &TempGenericBindingContext,
                                -1
                            )
                        );

                    m_ReportErrors = true;

                    if (TempNameBinding || NameIsBad)
                    {
                        Bindable::ValidateArity(
                            Name,
                            TempNameBinding,
                            TempGenericBindingContext->PGenericTypeBinding(),
                            GenericTypeArity,
                            GenericTypeArgsLoc ? GenericTypeArgsLoc : &Input->TextSpan,
                            m_Errors,
                            m_Compiler,
                            NameIsBad);
                    }

                    if (NameIsBad)
                    {
                        return AllocateBadExpression(Input->TextSpan);
                    }
                }

                // The name is not found. If it matches one of the old VB6
                // names, let people know where to find it. Otherwise, just
                // report it as undeclared.

                switch (Compiler::TokenOfString(Name))
                {
                    case tkOPEN:
                    case tkCLOSE:
                    case tkGET:
                    case tkPUT:
                    case tkPRINT:
                    case tkWRITE:
                    case tkINPUT:
                    case tkLOCK:
                    case tkUNLOCK:
                    case tkSEEK:
                    case tkWIDTH:
                    case tkNAME:
                    case tkFREEFILE:
                    case tkFEOF:
                    case tkLOC:
                    case tkLOF:
                        ReportSemanticError(
                            m_CompilerHost->IsStarliteHost() ?
                                ERRID_NoSupportFileIOKeywords1 : ERRID_ObsoleteFileIOKeywords1,
                            Input->TextSpan,
                            Input);
                        break;

                    case tkLINE:
                        ReportSemanticError(
                            m_CompilerHost->IsStarliteHost() ?
                                ERRID_NoSupportLineKeyword: ERRID_ObsoleteLineKeyword,
                            Input->TextSpan);
                        break;

                    case tkDEBUG:
                        ReportSemanticError(
                            ERRID_ObsoleteDebugKeyword1,
                            Input->TextSpan,
                            Input);
                        break;

                    case tkEMPTY:
                        ReportSemanticError(
                            ERRID_ObsoleteEmptyKeyword1,
                            Input->TextSpan,
                            Input);
                        break;

                    case tkNULL:
                        ReportSemanticError(
                            ERRID_ObsoleteNullKeyword1,
                            Input->TextSpan,
                            Input);
                        break;

                    case tkATN:
                        ReportSemanticError(
                            ERRID_ObsoleteMathKeywords2,
                            Input->TextSpan,
                            Input,
                            L"Atan");
                        break;

                    case tkSQR:
                        ReportSemanticError(
                            ERRID_ObsoleteMathKeywords2,
                            Input->TextSpan,
                            Input,
                            L"Sqrt");
                        break;

                    case tkSGN:
                        ReportSemanticError(
                            ERRID_ObsoleteMathKeywords2,
                            Input->TextSpan,
                            Input,
                            L"Sign");
                        break;

                    case tkAWAIT:
                        ReportBadAwaitInNonAsync(
                            Input->TextSpan);
                        break;

                    default:
                        ReportSemanticError(
                            m_InQuery ? ERRID_QueryNameNotDeclared : ERRID_NameNotDeclared1,
                            Input->TextSpan,
                            Input);

                        break;
                }

                // 
                return AllocateBadExpression(Input->TextSpan);
            }
        }

        STRING* MyDefaultInstanceBaseName = NULL;
        bool MangleName = false;
        if (!HasFlag(Flags, ExprAllowTypeReference) && NameBinding->IsType())
        {
            // if the name is type and type ref not allowed, give it a chance more in case the name is a
            // default instance.
            if( NameBinding->IsClass() &&
                (MyDefaultInstanceBaseName = GetDefaultInstanceBaseNameForMyGroupMember(NameBinding->PClass(), &MangleName)))
            {
                Flags |= ExprAllowTypeReference;
            }
            else
            {

                ReportSemanticError(
                    ERRID_TypeNotExpression1,
                    Input->TextSpan,
                    Input);

                // 
                return AllocateBadExpression(Input->TextSpan);
            }
        }

        if ((NameBinding->IsLocal() || NameBinding->IsStaticLocalBackingField()) &&
            // Parameter locals for synthesized methods sometimes have
            // bogus locations( i.e. the synthetic methods for events). There is
            // no foreseeable solution to provide location for such symbols.
            // Consider these clauses when this is no longer so.
            //
            NameBinding->IsVariable() &&
            !NameBinding->PVariable()->IsParameterLocal())
        {
            // Verify that the reference does not precede the point of
            // declaration of the symbol.

            const Location *Declaration = NameBinding->GetLocation();
            const Location *Reference = &Input->TextSpan;

            if (!(Declaration == NULL ||
                  Reference->m_lBegLine > Declaration->m_lBegLine ||
                  (Reference->m_lBegLine == Declaration->m_lBegLine &&
                   Reference->m_lBegColumn >= Declaration->m_lBegColumn) ||
                  m_PreserveExtraSemanticInformation))
            {
                ReportSemanticError(
                    ERRID_UseOfLocalBeforeDeclaration1,
                    Input->TextSpan,
                    NameBinding);
            }

            // Verify that we do not have a circular reference in initializer.

            if( OptionInferOn() )
            {
                InitializerInferInfo* circular = CircularReferenceInInitializer(NameBinding);

                if( circular )
                {
                    if( !circular->CircularReferenceDetected ||
                        !(m_Errors && m_Errors->HasThisErrorWithLocation(ERRID_CircularInference2, Input->TextSpan))) //(
                    {
                        ReportSemanticError(
                                ERRID_CircularInference2,
                                Input->TextSpan,
                                NameBinding,
                                NameBinding);
                    }

                    circular->CircularReferenceDetected = true;
                }
            }
        }

        Result =
            ReferToSymbol(
                Input->TextSpan,
                NameBinding,
                Input->AsName()->Name.TypeCharacter,
                NULL,
                GenericBindingContext,
                Flags);

        if (MyDefaultInstanceBaseName)
        {
            // check for default instance on base
            Result =
                CheckForDefaultInstanceProperty(
                    Input->TextSpan,
                    Result,
                    MyDefaultInstanceBaseName,
                    Flags,
                    MangleName);

            if(Result == 0 || IsBad(Result) )
            {
                ReportSemanticError(
                    ERRID_TypeNotExpression1,
                    Input->TextSpan,
                    Input);

                // 
                return AllocateBadExpression(Input->TextSpan);
            }
        }
    }
    return Result;
}

//
// This method is called from InterpretNameExpression to perform circular
// reference detection for inferred intiailizers and For Each. When we're doing
// inference with Option Infer, we must prevent circular references or we can't
// determine what type to infer for all cases. Consider:
//
//         Dim x = foo(x)
//
// This is an error. In fact, all uses of "x" inside the RHS will be an error.
// 
// In the future, we might be able to relax this contraint in some cases. For
// example:
// Dim x = Sub()
//               ...
//             x.Invoke()
//             ...
//         End Sub
// 
// In this case, the closed over name is used in such a way that it does not
// impact the lambda's inferred type. However, for now we use the simple rule
// that circularity isn't allowed, so this case is an error.
//
// The data structure used to track these is a stack of InitializerInferInfo
// structs. These structs are allocated on the C stack, and have a parent
// pointer to any outer initializers. The class inherits from BackupValue,
// which has the nice property that each stack entry is automatically popped
// when it goes out of scope. They can also be explicitly popped using the
// normal BackupValue mechanism.
//
// Since the data structure is already set up for us, the actual search is
// simply a walk up the stack to see if we have a maching name. If we do,
// it's a circular reference.
//
Semantics::InitializerInferInfo*
Semantics::CircularReferenceInInitializer
(
    _In_ Symbol* pNameBinding
)
{
    for (Semantics::InitializerInferInfo* i = m_InitializerInferStack; i != NULL; i = i->Parent)
    {
        if (i->Variable == pNameBinding)
        {
            return i;
        }
    }
    return NULL;
}

bool
Semantics::ShouldRebindExtensionCall
(
    ILTree::Expression * pQualifiedExpression,
    ExpressionFlags Flags
)
{
    if
    (
        pQualifiedExpression  &&
        ! HasFlag(Flags, ExprForceBaseReferenceToPropigatePropertyReference)
    )
    {
        if (pQualifiedExpression->bilop == SX_CALL && HasFlag32(pQualifiedExpression, SXF_CALL_WAS_EXTENSION_CALL))
        {
            ILTree::CallExpression * pCall = & pQualifiedExpression->AsCallExpression();

            if
            (
                pCall->Left &&
                pCall->Left->bilop == SX_SYM
            )
            {
                Procedure * pProc = ViewAsProcedure(pCall->Left->AsSymbolReferenceExpression().Symbol);

                //It's OK that we don't do generic type substitution here...
                //byref x as T is still byref, even if T hasn't been replaced with an actual type.
                if (pProc->GetFirstParam()->GetType()->IsPointerType())
                {
                    return true;
                }
            }
        }
        else if (pQualifiedExpression->bilop == SX_EXTENSION_CALL)
        {
            ExtensionCallLookupResult::iterator_type iter = pQualifiedExpression->AsExtensionCallExpression().ExtensionCallLookupResult->GetExtensionMethods();

            while (iter.MoveNext())
            {
                //It's OK that we don't do generic type substitution here...
                //byref x as T is still byref, even if T hasn't been replaced with an actual type.
                if (iter.Current().m_pProc->GetFirstParam()->GetType()->IsPointerType())
                {
                    return true;
                }
            }
        }
    }

    return false;

}


//If you are looking at a windiff trying to figure out what's going on
//with this file so that you can do an integration, here's the jist of it:
//
//InterpretGenericQualifiedExpression was renamed to
//InterpretGenericQualifiedSymbolExpression, and  then a new
//procedure named InterpretGenericQualifiedExpression was added.
//The new InterpretGenericQualifiedExpression contains contents that used to be under
//the Generic Qualified expression branch of InterpretExpression.
ILTree::Expression *
Semantics::InterpretGenericQualifiedExpression
(
        ParseTree::GenericQualifiedExpression *GenericQualified,
        ILTree::Expression * BaseReference,
        unsigned ArgumentCount,
        ExpressionFlags Flags,
        bool & CallerShouldReturnImmedietly
)
{
    ILTree::Expression * Result = NULL;
    bool ResultIsBad = IsBad(BaseReference);

    // Interpret the arguments.

    Type **BoundArguments = new(m_TreeStorage) Type *[ArgumentCount];
    Location *TypeArgumentLocations = new(m_TreeStorage) Location [ArgumentCount];

    unsigned TypeArgumentIndex = 0;

    for (ParseTree::TypeList *UnboundArguments = GenericQualified->Arguments.Arguments;
         UnboundArguments;
         UnboundArguments = UnboundArguments->Next)
    {
        bool TypeIsBad = false;
        BoundArguments[TypeArgumentIndex] = InterpretTypeName(UnboundArguments->Element, TypeIsBad)->DigThroughAlias();

        if (TypeIsBad)
        {
            ResultIsBad = true;
        }
        else
        {
            TypeArgumentLocations[TypeArgumentIndex] = UnboundArguments->Element->TextSpan;
        }

        TypeArgumentIndex++;
    }

    if (ResultIsBad)
    {
        CallerShouldReturnImmedietly = true;
        return AllocateBadExpression(GenericQualified->TextSpan);
    }

    if (BaseReference->bilop == SX_LATE_REFERENCE)
    {
        if (!GetFXSymbolProvider()->IsTypeAvailable(FX::TypeType))
        {
            ReportMissingType(FX::TypeType, GenericQualified->TextSpan);
            CallerShouldReturnImmedietly = true;
            return AllocateBadExpression(GenericQualified->TextSpan);
        }

        ArrayType *TypeArgumentArrayType = m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetTypeType());

        if (ArgumentCount > 0)
        {
            ExpressionList *TypeArguments;
            ExpressionList **Target = &TypeArguments;

            for (unsigned ArgumentIndex = 0; ArgumentIndex < ArgumentCount; ArgumentIndex++)
            {
                if (m_ReportErrors)
                {
                    // Check for restricted types in type arguments passed to late bound expression.
                    // 

                    CheckRestrictedType(
                        ERRID_RestrictedType1,
                        BoundArguments[ArgumentIndex],
                        &TypeArgumentLocations[ArgumentIndex],
                        m_CompilerHost,
                        m_Errors);
                }

                *Target =
                    AllocateExpression(
                        SX_LIST,
                        TypeHelpers::GetVoidType(),
                        AllocateExpression(
                            SX_METATYPE,
                            GetFXSymbolProvider()->GetTypeType(),
                            AllocateExpression(
                                SX_NOTHING,
                                BoundArguments[ArgumentIndex],
                                TypeArgumentLocations[ArgumentIndex]),
                            GenericQualified->TextSpan),
                        GenericQualified->TextSpan);

                Target = &(*Target)->AsExpressionWithChildren().Right;
            }

            BaseReference->AsExpressionWithChildren().Left->AsLateBoundExpression().TypeArguments =
                InitializeArray(
                    TypeArguments,
                    TypeArgumentArrayType,
                    NULL,
                    GenericQualified->TextSpan);

            if (IsBad(BaseReference->AsExpressionWithChildren().Left->AsLateBoundExpression().TypeArguments))
            {
                CallerShouldReturnImmedietly = true;
                return AllocateBadExpression(GenericQualified->TextSpan);
            }
        }
        return BaseReference;
    }
    else if (BaseReference->bilop == SX_EXTENSION_CALL)
    {
        //For extension calls we just store the type argument information
        //and then do the overload resolution later inside InterpretCallExpression
        ILTree::ExtensionCallExpression * extCall = &(BaseReference->AsExtensionCallExpression());

        extCall->TypeArguments = BoundArguments;
        extCall->TypeArgumentCount = ArgumentCount;
        extCall->TypeArgumentLocations = TypeArgumentLocations;

        CallerShouldReturnImmedietly = true;

        return
            ReferToExtensionMethod
            (
                GenericQualified->TextSpan,
                extCall,
                Flags,
                chType_NONE
            );
    }

    if (BaseReference->bilop != SX_SYM ||
        //
        // This is to catch cases like Class1(Of Integer)(Of Double). Although this is not allowed
        // by the parser, this could happen indirectly through imports aliases as A(Of Double) and
        // Imports A=Class1(Of Integer). In these cases, the symbol's binding context will be
        // a binding of the symbol itself.
        //
        (BaseReference->AsSymbolReferenceExpression().GenericBindingContext &&
            BaseReference->AsSymbolReferenceExpression().Symbol == BaseReference->AsSymbolReferenceExpression().GenericBindingContext->GetGeneric()))
    {
        ReportSemanticError(
            ERRID_ExpressionCannotBeGeneric1,
            GenericQualified->TextSpan,
            GenericQualified->Base);

        CallerShouldReturnImmedietly = true;
        return AllocateBadExpression(GenericQualified->TextSpan);
    }


    return
        InterpretGenericQualifiedSymbolExpression
        (
            GenericQualified,
            &(BaseReference->AsSymbolReferenceExpression()),
            BoundArguments,
            TypeArgumentLocations,
            ArgumentCount,
            Flags
        );
}



ILTree::Expression *
Semantics::InterpretExpression
(
    ParseTree::Expression *Input,
    ExpressionFlags Flags,
    int GenericTypeArity,
    Location *GenericTypeArgsLoc,
    BCSYM *TargetType  // Some things e.g. array literals can only be interpreted in a context where the target type is known
)
{
    // ==============================================================================
    //   !!!!!!!!!!!!!!      !!!!!!!!!!!!!!      !!!!!!!!!!!!!!      !!!!!!!!!!!!!!      
    //   !!! DANGER !!!      !!! DANGER !!!      !!! DANGER !!!      !!! DANGER !!!      
    //   !!!!!!!!!!!!!!      !!!!!!!!!!!!!!      !!!!!!!!!!!!!!      !!!!!!!!!!!!!!    
    // ==============================================================================
    //
    // "IntrepretExpression()" and "Convert()" are difficult to get right due to "linearity" and "mutability".
    // The functions have side effects -- modifying the SX_ tree, modifying the global error table
    // by reporting errors, and modifying type information within the SX_tree when binding
    // unbound things (like array literals and lambdas).
    //
    // Because of these side-effects, you generally only want to call the function ONCE on a given
    // expression tree. "Linearity" in general is the term for when you keep careful count of how
    // many times a given operation is done.
    //
    // But the design of the language forces this function to be called an UNKNOWN NUMBER OF TIMES.
    // Consider e.g.
    //
    // (1)   dim x = function(x)x+1
    // It calls IntrepretExpression. This turns it from SX_UNBOUNDLAMBDA to SX_LAMBDA, it alters
    // the p_typ parameter of the SX_PARAM for "x", it adds to the error table a message about
    // how Object was assumed. Note that type inference of lambdas REQUIRES those SX_PARAMS to be
    // alreayd modified.
    //
    // (2)   dim x as Func(int,int) = function(x)x+1
    // Here it calls IntrepretExpressionWithTargetType. This first calls InterpretExpression and
    // then calls Convert to convert it to a target type. The presence of a non-null TargetType,
    // and the expr flag DontInferResultType, together work to suppress the normal side effects.
    //
    // (3)   Sub f(Of T)(ByVal y as T)  ...  f(function(x)x+1)
    // Here it calls ResolveOverloadCandidates to do type inference on "f" and pick which of the
    // candidates was the best match. It sets the m_ReportErrors flag to false so as to suppress
    // the error-reporting side effect. It does InterpretExpression to infer a type hint, among
    // the other type hints it will receive. It calls InterpretExpression again to see if the function
    // can be converted to the chosen hint (I think). And once the overload has been chosen, it
    // calls InterpretCallExpression, which again calls InterpretExpression, but this time for real.
    //
    // (4)   dim x = {function(x)x+1}
    // Here the array literal calls InterpretExpression to infer the dominant type for the array
    // literal. A side-effect of this is to convert elements in the array literal TO that dominant
    // type. It's at this stage that errors get reported.
    //
    // (5)   dim x as Object() = {function(x)x+1}
    // Here we still infer a dominant type, but we can't report errors or convert the element to
    // that dominant type. This is controlled by DontInferResultType. When doing InterpretInitializer,
    // that first interprets the array literal, then it calls ConvertToTargetType, and it's within
    // Convert that the errors get reported and the SX_PARAM gets updated.
    //
    // (6)   f({function(x)x+1})
    // When doing method resolution, it calls ClassifyConversionFromElements. This has the side
    // effect of calling InterpretExpressionWithTargetType on each element to convert it. To prevent
    // this from modifying the SX_PARAMs, it's done on a scratch copy of the SX_tree.
    //
    // See also Dev10#470292 and http://bugcheck/default.asp?URL=/Bugs/DevDivBugs/153317.asp
    // and http://bugcheck/default.asp?URL=/Bugs/DevDivBugs/135729.asp for three bugs relating to these cases.
    //
    // The delicate dance of m_ReportErrors, DontInferResultsType, xShallowCopyLambdaTree, has to be
    // done exactly right. But it's difficult because there are so many different paths through
    // InterpretExpression. And it's inelegant because the contextual information about whether to
    // have side effects has to permeate through to even the most basic functions. For instance,
    // the simple function ConvertArrayLiteralElement has to know whether its caller wanted the
    // conversion to be a permanent one that affects the SX_PARAM nodes of function elements,
    // or a scratch one that gets thrown away.
    //
    // There aren't local refactorings we can do to improve the situation. The only thing that will help
    // is to switch to immutable datastructures. The error table should be considered immutable, and the
    // entire SX_tree (including inferred types) should be considered immutable. If a function needs to
    // update any of these things, then it should create a new copy and return that. That way, for instance,
    // method resolution would just be able to interpret whole (tree+errortables) to see which ones worked,
    // and throw away the ones that didn't, and preserve the tree+errortables for the ones that did.
    // 


    ILTree::Expression *Result = NULL;

    VSASSERT(!HasFlag(Flags, ExprForceConstructorCall) || Input->Opcode == ParseTree::Expression::CallOrIndex,
                        "Expected ExprForceConstructorCall flag only for CallOrIndex.");
    VSASSERT(!HasFlag(Flags, ExprIsQueryOperator) || Input->Opcode == ParseTree::Expression::DotQualified || Input->Opcode == ParseTree::Expression::GenericQualified,
                        "Expected ExprIsQueryOperator flag only for DotQualified or GenericQualified.");

    // remember error count to see if errors got introduced to prevent duplicate errors when re-interpreting
    // for lambda type inference.
    unsigned NumberOfErrors = m_Errors ? m_Errors->GetErrorCount() : 0;

    BackupValue<bool> backup_m_InConstantExpressionContext(&m_InConstantExpressionContext);
    if (HasFlag(Flags, ExprMustBeConstant))
    {
        // Temporary fix for 








        m_InConstantExpressionContext = true;
    }

    switch (Input->Opcode)
    {
        case ParseTree::Expression::SyntaxError:
        {
            return AllocateBadExpression(Input->TextSpan);
        }

        case ParseTree::Expression::AlreadyBound:
        {
            //This is just a placeholder for a bound expression that was already interpreted.
            //Just return the BILTREE Expression.
            Result = Input->AsAlreadyBound()->BoundExpression;

            if(IsBad(Result))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            break;
        }

        case ParseTree::Expression::AlreadyBoundSymbol:
        {
            GenericBinding *GenericBindingContext = NULL;
            Symbol * BoundSymbol = Input->AsAlreadyBoundSymbol()->Symbol;

            Type *ReferencedClass;
            if (Input->AsAlreadyBoundSymbol()->BaseReference == NULL)
            {
                ReferencedClass = BoundSymbol->PNamedRoot()->GetContainingClass();
            }
            else
            {
                ReferencedClass = BoundSymbol->PNamedRoot()->GetContainingClassOrInterface();
            }
            ThrowIfNull(ReferencedClass);

            ILTree::Expression * BaseReference = NULL;
            if (Input->AsAlreadyBoundSymbol()->BaseReference != NULL)
            {
                BaseReference = InterpretExpression(Input->AsAlreadyBoundSymbol()->BaseReference, Flags & ~(ExprIsAssignmentTarget | ExprAccessDefaultProperty | ExprPropagatePropertyReference | ExprIsExplicitCallTarget));
            }

            if (BoundSymbol->IsGenericBinding())
            {
                GenericBindingContext = BoundSymbol->PGenericBinding();
                ReferencedClass = GenericBindingContext;
            }
            else if (BaseReference!=NULL && Input->AsAlreadyBoundSymbol()->UseBaseReferenceTypeAsSymbolContainerType && 
                    BaseReference->ResultType!=NULL && BaseReference->ResultType->IsGenericBinding())
            {
                // e.g. IEnumerable<int>.currentField
                // In this case, the generic binding context for "currentField" should be based off the concrete binding of its base reference.
                GenericBindingContext = BaseReference->ResultType->PGenericBinding();
              
                // ReferencedClass = xxxx. (note: the "ReferencedClass" that is mentioned elsewhere in AlreadyBoundSymbol case isn't actually used!)
            }
            else if (ReferencedClass != NULL && IsGenericOrHasGenericParent(ReferencedClass->PContainer()))
            {
                GenericBindingContext = SynthesizeOpenGenericBinding(ReferencedClass->PClass(), m_SymbolCreator);
                ReferencedClass = GenericBindingContext;
            }


            //This is just a placeholder for a bound expression that was already interpreted.
            //Just return the BILTREE Expression.
            Result = ReferToSymbol(
                    Input->TextSpan,
                    BoundSymbol,
                    chType_NONE,
                    BaseReference,
                    GenericBindingContext,
                    Flags);
                break;
        }

        case ParseTree::Expression::Parenthesized:
        {
            ExpressionFlags OperandFlags = Flags | ExprForceRValue;
            ClearFlag(OperandFlags, ExprIsAssignmentTarget | ExprAccessDefaultProperty | ExprPropagatePropertyReference | ExprIsExplicitCallTarget );

            Result = InterpretExpression(Input->AsUnary()->Operand, OperandFlags);
            if (Result->ResultType->IsArrayLiteralType() && Result->bilop == SX_ARRAYLITERAL)
            {
                // Convert the array literal to its inferred array type, reporting warnings/errors if necessary
                Result = ConvertArrayLiteral(&Result->AsArrayLiteralExpression(), NULL);
            }

            Result->Loc = Input->TextSpan;                
            SetFlag32(Result, SXF_PAREN_EXPR);

            break;
        }

        case ParseTree::Expression::New:
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            bool TypeIsBad = false;
            Type *TypeOfInstance =
                InterpretTypeName(Input->AsNew()->InstanceType, TypeIsBad);

            if (TypeIsBad)
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            Result =
                CreateConstructedInstance(
                    TypeOfInstance,
                    Input->AsNew()->InstanceType->TextSpan,
                    Input->TextSpan,
                    Input->AsNew()->Arguments.Values,
                    Flags);

            break;
        }

        case ParseTree::Expression::NewArrayInitializer:
        {
            // The expression creates an array.

            if (HasFlag(Flags, ExprMustBeConstant) &&
                // Although only constants are allowed in applied attribute contexts, 1-D arrays are allowed too
                //
                !IsAppliedAttributeContext())
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            ParseTree::NewArrayInitializerExpression *NewArrayExpr = Input->AsNewArrayInitializer();
            ExpressionList *DimensionSizes = NULL;

            AssertIfNull(NewArrayExpr->ArrayType);

            if (NewArrayExpr->ArrayType->Opcode == ParseTree::Type::ArrayWithSizes)
            {
                bool SomeDimensionsBad = false;

                DimensionSizes =
                    InterpretArraySizeList(
                        NewArrayExpr->ArrayType->AsArrayWithSizes()->Dims,
                        Flags,
                        SomeDimensionsBad);

                if (SomeDimensionsBad)
                {
                    return AllocateBadExpression(Input->TextSpan);
                }
            }

            bool TypeIsBad = false;
            Type *ResultType = InterpretTypeName(NewArrayExpr->ArrayType, TypeIsBad);
            Location &ResultTypeLocation = NewArrayExpr->ArrayType->TextSpan;

            CheckRestrictedArrayType(ResultType, &(NewArrayExpr->ArrayType->TextSpan), m_CompilerHost, m_Errors);

            if (TypeIsBad)
            {
                return AllocateBadExpression(NewArrayExpr->TextSpan);
            }

            ExpressionList *Initializer =
                InterpretArrayInitializerList(
                    NewArrayExpr->Elements,
                    (Flags & ExprMustBeConstant) | ExprForceRValue);

            if (NULL == Initializer || IsBad(Initializer))
            {
                return AllocateBadExpression(NewArrayExpr->TextSpan);
            }

            Result =
                InitializeArray(
                    Initializer,
                    ResultType->PArrayType(),
                    DimensionSizes,
                    NewArrayExpr->TextSpan);

            break;
        }

        case ParseTree::Expression::NewObjectInitializer:
        {
            if (HasFlag(Flags, ExprMustBeConstant) || m_InConstantExpressionContext)
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            ParseTree::NewObjectInitializerExpression *NewObjectInit = Input->AsNewObjectInitializer();

            if (NewObjectInit->NewExpression)
            {
                ILTree::Expression *ConstructedInstance =
                    InterpretExpression(NewObjectInit->NewExpression, Flags);

                AssertIfTrue(NewObjectInit->NoWithScope);
                AssertIfTrue(NewObjectInit->QueryErrorMode);

                if (NULL == ConstructedInstance || IsBad(ConstructedInstance))
                {
                    return AllocateBadExpression(NewObjectInit->TextSpan);
                }

                // Get the location for the clause beginning with the "With"
                // keyword till the end of initalizer list.
                Location TextSpanOfWithClause;
                TextSpanOfWithClause.m_lBegLine = NewObjectInit->TextSpan.m_lBegLine + NewObjectInit->With.Line;
                TextSpanOfWithClause.m_lBegColumn = NewObjectInit->With.Column;
                TextSpanOfWithClause.m_lEndLine = NewObjectInit->TextSpan.m_lEndLine;
                TextSpanOfWithClause.m_lEndColumn = NewObjectInit->TextSpan.m_lEndColumn;

                Result =
                    CreateInitializedObject(
                        NewObjectInit->InitialValues,
                        ConstructedInstance,
                        NewObjectInit->TextSpan,
                        TextSpanOfWithClause,
                        Flags);
            }
            else
            {
                Result =
                    InitializeAnonymousType(
                        NewObjectInit->InitialValues,
                        NewObjectInit->NoWithScope,
                        NewObjectInit->QueryErrorMode,
                        m_Project ?
                            m_Compiler->GetUnnamedNamespace(m_Project):
                            m_Compiler->GetUnnamedNamespace(),  // Generate anonymous types in global namespace.
                        NULL,
                        NewObjectInit->GetLocationOfNew(),
                        NewObjectInit->TextSpan,
                        Flags);
            }
            break;
        }

        case ParseTree::Expression::FloatingLiteral:
        {
            Type *ResultType;

            if (Input->AsFloatingLiteral()->TypeCharacter == chType_NONE)
            {
                ResultType = GetFXSymbolProvider()->GetDoubleType();
            }
            else
            {
                ResultType = GetFXSymbolProvider()->GetType(
                    VtypeOfTypechar(Input->AsFloatingLiteral()->TypeCharacter));
            }

            // If the literal's type is smaller than double, the value needs to be narrowed.
            // Overflow will have been determined during scanning and is ignored here.

            bool Overflow = false;

            Result =
                ProduceFloatingConstantExpression(
                    NarrowFloatingResult(
                        Input->AsFloatingLiteral()->Value,
                        ResultType,
                        Overflow),
                    Input->TextSpan,
                    ResultType
                    IDE_ARG(0));
            break;
        }

        case ParseTree::Expression::DateLiteral:
        {
            Result =
                ProduceConstantExpression(
                    Input->AsDateLiteral()->Value,
                    Input->TextSpan,
                    GetFXSymbolProvider()->GetDateType()
                    IDE_ARG(0));
            break;
        }

        case ParseTree::Expression::IntegralLiteral:
        {
            Type *ResultType;

            if (Input->AsIntegralLiteral()->TypeCharacter == chType_NONE)
            {
                if (Input->AsIntegralLiteral()->Base ==
                        ParseTree::IntegralLiteralExpression::Decimal)
                {
                    ResultType =
                        Input->AsIntegralLiteral()->Value > (Quadword)0x7fffffff ?
                            GetFXSymbolProvider()->GetLongType() :
                            GetFXSymbolProvider()->GetIntegerType();
                }
                else
                {
                    ResultType =
                        (unsigned __int64)Input->AsIntegralLiteral()->Value > (unsigned __int64)0xffffffff ?
                            GetFXSymbolProvider()->GetLongType() :
                            GetFXSymbolProvider()->GetIntegerType();
                }
            }
            else
            {
                ResultType = GetFXSymbolProvider()->GetType(
                    VtypeOfTypechar(Input->AsIntegralLiteral()->TypeCharacter));
            }

            // If the literal is non-decimal and has a result type smaller than Long,
            // its value may actually be negative. Overflow is ignored in such cases.

            bool Overflow = false;

            Result =
                ProduceConstantExpression(
                    NarrowIntegralResult(
                        Input->AsIntegralLiteral()->Value,
                        ResultType,
                        ResultType,
                        Overflow),
                    Input->TextSpan,
                    ResultType
                    IDE_ARG(0));

            SetFlag32(Result, SXF_ICON_LITERAL);
            break;
        }

        case ParseTree::Expression::DecimalLiteral:
            Result =
                ProduceDecimalConstantExpression(
                    Input->AsDecimalLiteral()->Value,
                    Input->TextSpan
                    IDE_ARG(0));
            break;

        case ParseTree::Expression::CharacterLiteral:

            Result =
                ProduceConstantExpression(
                    Input->AsCharacterLiteral()->Value,
                    Input->TextSpan,
                    GetFXSymbolProvider()->GetCharType()
                    IDE_ARG(0));
            break;

        case ParseTree::Expression::BooleanLiteral:

            Result =
                ProduceConstantExpression(
                    Input->AsBooleanLiteral()->Value ? COMPLUS_TRUE : COMPLUS_FALSE,
                    Input->TextSpan,
                    GetFXSymbolProvider()->GetBooleanType()
                    IDE_ARG(0));
            break;

        case ParseTree::Expression::StringLiteral:
        case ParseTree::Expression::XmlCharData:
        case ParseTree::Expression::XmlReference:
        {
            size_t Length = Input->AsStringLiteral()->LengthInCharacters;
            WCHAR *Spelling = new (m_TreeStorage) WCHAR [Length + 1];
            memcpy(Spelling, Input->AsStringLiteral()->Value, (Length + 1) * sizeof(WCHAR));

            Result =
                ProduceStringConstantExpression(
                    Spelling,
                    Length,
                    Input->TextSpan
                    IDE_ARG(0));
            break;
        }

        case ParseTree::Expression::Nothing:

            Result = AllocateExpression(SX_NOTHING, GetFXSymbolProvider()->GetObjectType(), Input->TextSpan);
            break;

        case ParseTree::Expression::Deferred:

            return InterpretExpression(Input->AsDeferred()->Value, Flags);

        case ParseTree::Expression::MyClass:
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            if (WithinModule())
            {
                ReportSemanticError(
                    ERRID_MyClassNotInClass,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            if (!WithinInstanceProcedure())
            {
                ReportSemanticError(
                    ERRID_UseOfKeywordNotInInstanceMethod1,
                    Input->TextSpan,
                    L"MyClass");

                return AllocateBadExpression(Input->TextSpan);
            }

            // Within a non-shared method, MyClass is equivalent to Me
            // (except that it disables virtual calls).

            Result =
                AllocateSymbolReference(
                    ContainingClass()->GetMe(),
                    ContainingClass(),
                    NULL,
                    Input->TextSpan);
            SetFlag32(Result, SXF_SYM_MYCLASS);

            if (m_DisallowMeReferenceInConstructorCall)
            {
                ReportSemanticError(
                    ERRID_InvalidMeReference,
                    Input->TextSpan);
            }

            break;
        }

        case ParseTree::Expression::Me:
        case ParseTree::Expression::MyBase:
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            if (!WithinInstanceProcedure() && !m_IsGeneratingXML)
            {
                ReportSemanticError(
                    WithinModule() ?
                    ERRID_UseOfKeywordFromModule1 :
                    ERRID_UseOfKeywordNotInInstanceMethod1,
                    Input->TextSpan,
                    Input->Opcode == ParseTree::Expression::Me ? L"Me" : L"MyBase");

                return AllocateBadExpression(Input->TextSpan);
            }

            Type *ReferencedClass = ContainingClass();

            if (ReferencedClass == NULL)
            {
                VSASSERT(
                    Input->Opcode == ParseTree::Expression::MyBase,
                    "Expected MyBase expression isn't.");

                ReportSemanticError(
                    ERRID_UseOfKeywordOutsideClass1,
                    Input->TextSpan,
                    L"MyBase");

                return AllocateBadExpression(Input->TextSpan);
            }

            GenericBinding *GenericBindingContext = NULL;
            if (IsGenericOrHasGenericParent(ReferencedClass->PClass()))
            {
                GenericBindingContext = CreateMeGenericBinding(ReferencedClass->PClass());
                ReferencedClass = GenericBindingContext;
            }

            if (Input->Opcode == ParseTree::Expression::MyBase)
            {
                if (TypeHelpers::IsRecordType(ReferencedClass) || ReferencedClass->PClass()->IsStdModule())
                {
                    ReportSemanticError(
                        TypeHelpers::IsRecordType(ReferencedClass) ?
                            ERRID_UseOfKeywordFromStructure1 :
                            ERRID_UseOfKeywordFromModule1,
                        Input->TextSpan,
                        L"MyBase");

                    return AllocateBadExpression(Input->TextSpan);
                }

                ReferencedClass = GetBaseClass(ReferencedClass);

                if (ReferencedClass == NULL)
                {
                    // There was an error in declaring the class.
                    return AllocateBadExpression(Input->TextSpan);
                }

                GenericBindingContext = TypeHelpers::IsGenericTypeBinding(ReferencedClass) ? ReferencedClass->PGenericTypeBinding() : NULL;
            }
            else
            {
                VSASSERT(Input->Opcode == ParseTree::Expression::Me, "Expected Me.");
            }

            if (Input->Opcode == ParseTree::Expression::Me)
            {
                if ((HasFlag(Flags, ExprIsAssignmentTarget) &&  
                      !HasFlag(Flags, ExprAccessDefaultProperty))  ||
                     ReferencedClass->IsEnum())    // IDE only so doesn't affect vbc.exe
            {
                ReportSemanticError(
                    ERRID_InvalidMe,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
                }
            }

            Result =
                AllocateSymbolReference(
                    ContainingClass()->GetMe(),
                    ReferencedClass,
                    NULL,
                    Input->TextSpan);
            Result->AsSymbolReferenceExpression().GenericBindingContext = GenericBindingContext;

            if (Input->Opcode == ParseTree::Expression::MyBase)
            {
                SetFlag32(Result, SXF_SYM_MYBASE);
            }

            if (m_DisallowMeReferenceInConstructorCall)
            {
                ReportSemanticError(
                    ERRID_InvalidMeReference,
                    Input->TextSpan);
            }

            break;
        }

        case ParseTree::Expression::GlobalNameSpace:
            {
                Result =
                    ReferToSymbol(
                        Input->TextSpan,
                        m_Project ?
                            m_Compiler->GetUnnamedNamespace(m_Project):
                            m_Compiler->GetUnnamedNamespace(),
                        chType_NONE,    // type character
                        NULL,
                        NULL,
                        Flags);
                break;
            }

        case ParseTree::Expression::Name:
        {
            BackupValue<Type *> backup_receiver_type(&m_pReceiverType);
            BackupValue<Location *> backup_receiver_loaction(&m_pReceiverLocation);

            if (!m_pReceiverType && (ContainingClass() && !ContainingClass()->IsStdModule()))
            {
                m_pReceiverType = ContainingClass();
                m_pReceiverLocation = &(Input->TextSpan);
            }

            Result = InterpretNameExpression
            (
                Input,
                Flags,
                GenericTypeArity,
                GenericTypeArgsLoc
            );

            if (IsBad(Result))
            {
                return Result;
            }

            // Not allowed to refer to a function's "Implicit Return Variable" in an async/iterator method...
            if (Result!=NULL && Result->bilop == SX_SYM && m_ProcedureTree!=NULL && m_ProcedureTree->ReturnVariable == Result->AsSymbolReferenceExpression().Symbol)
            {
                if (m_ProcedureTree->m_ResumableKind == ILTree::TaskResumable ||
                    m_ProcedureTree->m_ResumableKind == ILTree::IteratorResumable ||
                    m_ProcedureTree->m_ResumableKind == ILTree::IterableResumable)
                {
                    ReportSemanticError(ERRID_BadResumableAccessReturnVariable, Result->Loc);
                    return AllocateBadExpression(Result->Loc);
                }
            }

            break;
        }

        case ParseTree::Expression::DotQualified:
        case ParseTree::Expression::BangQualified:
        case ParseTree::Expression::XmlElementsQualified:
        case ParseTree::Expression::XmlAttributeQualified:
        case ParseTree::Expression::XmlDescendantsQualified:
        {
            BackupValue<Type *> backup_receiver_type(&m_pReceiverType); //this variable will restore the value of
                                                                        //m_pReceiverType when it goes out of scope.
            BackupValue<Location *> backup_receiver_location(&m_pReceiverLocation);
            TemporaryErrorTable temporary_error_table(m_Compiler, &m_Errors); //restores the value m_Errors whenver it goes of out scope.
                                                                        //it will also by default merge in the current temporary table (if it exists) before doing the restore.

            // !!! DANGER !!!
            // See the comments in TemporaryErrorTable.Restore.
            // In general, InterpretExpression can have side-effects on the SX_ tree as well (in the case of
            // unbound lambdas function(x)x), which we might have had to to merge. Luckily for us, though,
            // the only InterpretExpression that we're doing is simple name references so that doesn't apply.
            //
            bool lookingForAQueryOperator = HasFlag(Flags, ExprIsQueryOperator);
            ClearFlag(Flags, ExprIsQueryOperator);

            if (m_Errors && ! HasFlag(Flags, ExprForceBaseReferenceToPropigatePropertyReference))
            {
                temporary_error_table.AddTemporaryErrorTable(new ErrorTable(*m_Errors));
                temporary_error_table.EnableMergeOnRestore(0);
                m_Errors = temporary_error_table.NewErrorTable(0);
            }

            ILTree::Expression *BaseReference = NULL;

            if (Input->AsQualified()->Base)
            {
                // Don't pass down the ExprMustBeConstant flag, because it is possible
                // to refer to a constant using a non-constant base reference.
                //
                // The ultimate disposition of the qualified expression must deal
                // with checking for non-constants.
                //
                // Don't pass down the ExprSuppressTypeArgumentsChecking flag because
                // it should apply only to the qualified name and not to the base
                // expression.
                //

                BaseReference =
                    InterpretExpression(
                        Input->AsQualified()->Base,
                        Input->Opcode != ParseTree::Expression::DotQualified ?
                            ExprNoFlags :
                            ((ExprAllowTypeReference |
                              ExprAllowNamespaceReference |
                              ExprSuppressImplicitVariableDeclaration |
                              ExprLeadingQualifiedName |
                              (((Flags & ExprForceBaseReferenceToPropigatePropertyReference) == ExprForceBaseReferenceToPropigatePropertyReference) ? ExprPropagatePropertyReference : ExprNoFlags) |
                              (Flags & ExprTypeReferenceOnly & ~ExprSuppressTypeArgumentsChecking) |
                              ((Flags & ExprMustBeConstant) ? ExprNoFlags : ExprNoFlags))
                              | (ExprNoFlags)));

                if (BaseReference->ResultType->IsArrayLiteralType() && BaseReference->bilop == SX_ARRAYLITERAL)
                {
                    // Convert the array literal to its inferred array type, reporting warnings/errors if necessary
                    BaseReference = ConvertArrayLiteral(&BaseReference->AsArrayLiteralExpression(), NULL);
                }

                // base can be resolved to both to an expression and a type. Help to disambiguate base.sharedMemeber
                // consider only property.sharedMember or variable.sharedMember
                if (!m_EvaluatingConditionalCompilationConstants &&
                    Input->AsQualified()->Base->Opcode == ParseTree::Expression::Name &&
                    BaseReference->bilop != SX_NAME_NOT_FOUND &&
                    !IsBad(BaseReference) &&
                    (( BaseReference->bilop == SX_SYM && BaseReference->AsSymbolReferenceExpression().Symbol->IsVariable()) ||    // variable, or
                     (BaseReference->bilop == SX_CALL &&                                                    // property
                        BaseReference->AsCallExpression().Left->bilop == SX_SYM &&
                        BaseReference->AsCallExpression().Left->AsSymbolReferenceExpression().Symbol->IsProc() &&
                        IsPropertyGet(BaseReference->AsCallExpression().Left->AsSymbolReferenceExpression().Symbol->PProc()))) &&
                    BaseReference->ResultType->IsNamedRoot() &&
                    StringPool::IsEqual(BaseReference->ResultType->PNamedRoot()->GetName(),
                                        Input->AsQualified()->Base->AsName()->Name.Name ))
                {

                    bool NameTypeIsBad = false;
                    Identifier *NameAsType = Input->AsQualified()->Base->AsName()->Name.Name;

                    NameFlags NameAsTypeLookupFlags = NameSearchTypeReferenceOnly | NameSearchLeadingQualifiedName | NameSearchIgnoreExtensionMethods;

                    GenericBinding *GenericBindingContext = NULL;

                    BackupValue<bool> backup_report_errors(&m_ReportErrors);
                    m_ReportErrors = false;

                    Declaration *NameBindingAsType =
                        EnsureNamedRoot
                        (
                            InterpretName
                            (
                                NameAsType,
                                m_Lookup,
                                NULL,   // No Type parameter lookup
                                NameAsTypeLookupFlags,
                                ContainingClass(), 
                                Input->AsQualified()->Base->TextSpan,
                                NameTypeIsBad,
                                &GenericBindingContext,
                                GenericTypeArity
                            )
                        );

                    if (NameBindingAsType && !NameTypeIsBad)
                    {
                        BaseReference->NameCanBeType = true;
                    }

                }
            }
            else
            {
                // ".Member" case, where base expression is specified in "With" block
                BaseReference = EnclosingWithValue(Input->TextSpan, Flags);
            }

            //Save the result type of the base reference so that extension method
            //name lookup can use it to apply the super-type filter to extension methods.
            m_pReceiverType = BaseReference->ResultType;
            m_pReceiverLocation = &BaseReference->Loc;

#if IDE  
            if (BaseReference->bilop == SX_NAME_NOT_FOUND)
            {
                // The base reference is a name that was not found. Try looking up
                // the fully qualified form. (This occurs only when evaluating under
                // the debugger.)

                bool GlobalQualified = false;
                Identifier *MemberName = SynthesizeQualifiedName(Input->AsQualified(), GlobalQualified);

                NameFlags NameLookupFlags = NameNoFlags;
                GenericBinding *GenericBindingContext = NULL;
                Scope *Lookup = NULL;
                bool NameIsBad = false;

                if (GlobalQualified)
                {
                    Lookup =
                            m_Project ?
                                m_Compiler->GetUnnamedNamespace(m_Project)->GetHash():
                                m_Compiler->GetUnnamedNamespace()->GetHash();

                    NameLookupFlags |= NameSearchIgnoreParent | NameSearchIgnoreImports;

                }
                else
                {
                    Lookup = m_Lookup;
                }

                if (HasFlag(Flags, ExprTypeReferenceOnly))
                {
                    SetFlag(NameLookupFlags, NameSearchTypeReferenceOnly);
                }

                Declaration *Member =
                    EnsureNamedRoot
                    (
                        InterpretName
                        (
                            MemberName,
                            Lookup,
                            NULL,   // No Type parameter lookup
                            NameSearchIgnoreExtensionMethods ,
                            NULL,
                            Input->TextSpan,
                            NameIsBad,
                            &GenericBindingContext,
                            GenericTypeArity
                        )
                    );

                if (NameIsBad)
                {
                    return AllocateBadExpression(Input->TextSpan);
                }

                if (Member)
                {
                    if (!HasFlag(Flags, ExprAllowTypeReference) && Member->IsType())
                    {
                        ReportSemanticError(
                            ERRID_TypeNotExpression1,
                            Input->TextSpan,
                            Input);

                        return AllocateBadExpression(Input->TextSpan);
                    }

                    VSASSERT(Input->AsQualified()->Name != NULL &&
                             Input->AsQualified()->Name->Opcode == ParseTree::Expression::Name,
                             "QualifiedExpression::Name must be set to ILTree::ArgumentNameExpression!");
                    Result =
                        ReferToSymbol(
                        Input->TextSpan,
                        Member,
                        Input->AsQualified()->Name->AsName()->Name.TypeCharacter,
                        NULL,
                        GenericBindingContext,
                        Flags);
                }
                else
                {
                    if (HasFlag(Flags, ExprTreatQualifiedNamesEnMasse))
                    {
                        return
                            AllocateExpression(
                                SX_NAME_NOT_FOUND,
                                TypeHelpers::GetVoidType(),
                                Input->TextSpan);
                    }

                    ReportSemanticError(
                        ERRID_NameNotDeclaredDebug1,
                        Input->TextSpan,
                        MemberName);

                    return AllocateBadExpression(Input->TextSpan);
                }

                break;
            }
#endif
            if (IsBad(BaseReference))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            //create a scope to control the life of UseQueryNameLookup_backup
            {
                BackupValue<bool> UseQueryNameLookup_backup(&m_UseQueryNameLookup);
                m_UseQueryNameLookup = false;

                Result =
                    InterpretQualifiedExpression(
                        BaseReference,
                        Input->AsQualified()->Name,
                        Input->Opcode,
                        Input->TextSpan,
                        Flags |
                        (lookingForAQueryOperator ? ExprIsQueryOperator : ExprNoFlags)
                        ,
                        GenericTypeArity);

                if (ShouldRebindExtensionCall(Result, Flags))
                {
                    temporary_error_table.SuppressMergeOnRestore();
                    temporary_error_table.Restore();

                    Result =
                        InterpretExpression
                        (
                            Input,
                            Flags  | ExprForceBaseReferenceToPropigatePropertyReference |
                            (lookingForAQueryOperator ? ExprIsQueryOperator : ExprNoFlags)
                            ,
                            GenericTypeArity,
                            GenericTypeArgsLoc
                        );

                    return Result;
                }
                else
                {
                    temporary_error_table.Restore();
                }
            }

            break;
        }

        case ParseTree::Expression::GenericQualified:
        {
            ParseTree::GenericQualifiedExpression *GenericQualified = Input->AsGenericQualified();

            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            // Count the arguments.
            unsigned ArgumentCount = 0;
            for (ParseTree::TypeList *ArgumentsToCount = GenericQualified->Arguments.Arguments;
                 ArgumentsToCount;
                 ArgumentsToCount = ArgumentsToCount->Next)
            {
                ArgumentCount++;
            }

            ILTree::Expression *BaseReference =
                InterpretExpression(
                    GenericQualified->Base,
                    (Flags & (ExprAllowTypeReference | ExprTypeReferenceOnly | ExprIsQueryOperator)) |
                        ExprIsExplicitCallTarget |
                        ExprSuppressImplicitVariableDeclaration |
                        ExprSuppressTypeArgumentsChecking |
                        ExprPropagatePropertyReference,
                    ArgumentCount,
                    GenericQualified->Arguments.Arguments ? &GenericQualified->Arguments.Arguments->TextSpan : NULL);

            //If you are looking at a windiff trying to figure out what's going on
            //with this file so that you can do an integration, here's the jist of it:
            //
            //InterpretGenericQualifiedExpression was renamed to
            //InterpretGenericQualifiedSymbolExpression, and  then a new
            //procedure named InterpretGenericQualifiedExpression was added.
            //The new InterpretGenericQualifiedExpression contains contents that used to be under
            //the Generic Qualified expression branch of InterpretExpression.

            bool shouldReturnImmedietly = false;

            Result =
                InterpretGenericQualifiedExpression
                (
                    GenericQualified,
                    BaseReference,
                    ArgumentCount,
                    Flags & (~ExprIsQueryOperator),
                    shouldReturnImmedietly
                );

            if (shouldReturnImmedietly)
            {
                return Result;
            }
            else
            {
                break;
            }

        }

        case ParseTree::Expression::CallOrIndex:

        {
            ParseTree::CallOrIndexExpression *CallOrIndex = Input->AsCallOrIndex();
            typeChars TypeCharacter = ExtractTypeCharacter(CallOrIndex->Target);

            Result =
                InterpretCallOrIndex
                (
                    CallOrIndex,
                    Flags,
                    TypeCharacter
                );
            break;
        }

        case ParseTree::Expression::IsType:
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            ILTree::Expression *Value =
                InterpretExpression(
                    Input->AsTypeValue()->Value,
                    ExprForceRValue);

            // A test for IsValueType would not be correct here, because
            // it would let Void types slip through.

            if (!IsBad(Value) &&
                !TypeHelpers::IsReferenceType(Value->ResultType) &&
                !TypeHelpers::IsGenericParameter(Value->ResultType))     // 
            {
                ReportSemanticError(
                    ERRID_TypeOfRequiresReferenceType1,
                    Value->Loc,
                    Value->ResultType);

                MakeBad(Value);
            }

            bool TypeIsBad = false;

            Type *IsType =
                InterpretTypeName(
                    Input->AsTypeValue()->TargetType,
                    TypeIsBad);

            if (IsBad(Value) || TypeIsBad)
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            VSASSERT(TypeHelpers::IsReferenceType(Value->ResultType) || TypeHelpers::IsGenericParameter(Value->ResultType),
                        "How did a non-reference type or non-generic param type get here?");

            // 


            if (ClassifyTryCastConversion(IsType, Value->ResultType) == ConversionError)
            {
                // Expression of type '|1' can never be of type '|2'.

                if (m_ReportErrors && m_Errors)
                {
                    CompilerProject *SourceTypeProject = NULL;
                    CompilerProject *TargetTypeProject = NULL;

                    ConversionClass MixedProjectClassification =
                        ClassifyTryCastConversion(
                            IsType,
                            Value->ResultType,
                            true,
                            &TargetTypeProject,
                            &SourceTypeProject);

                    if (MixedProjectClassification != ConversionError &&
                        SourceTypeProject && SourceTypeProject != m_Project &&
                        TargetTypeProject && TargetTypeProject != m_Project &&
                        SourceTypeProject != TargetTypeProject)
                    {
                        StringBuffer TextBuffer1;
                        StringBuffer TextBuffer2;

                        ReportSmartReferenceError(
                            ERRID_TypeOfExprAlwaysFalse2,
                            m_Project,
                            SourceTypeProject,
                            m_Compiler,
                            m_Errors,
                            SourceTypeProject->GetFileName(),     // 
                            &Input->TextSpan,
                            ExtractErrorName(Value->ResultType, TextBuffer1),
                            ExtractErrorName(IsType, TextBuffer1));
                    }
                    else
                    {
                        ReportSemanticError(
                            ERRID_TypeOfExprAlwaysFalse2,
                            Input->TextSpan,
                            Value->ResultType,
                            IsType);
                    }
                }

                return AllocateBadExpression(Input->TextSpan);
            }

            Result =
                AllocateExpression(
                    SX_ISTYPE,
                    GetFXSymbolProvider()->GetBooleanType(),
                    TypeHelpers::IsGenericParameter(Value->ResultType) ?
                        AllocateExpression(
                            SX_DIRECTCAST,
                            GetFXSymbolProvider()->GetObjectType(),
                            Value,
                            Value->Loc) :
                        Value,
                    // The only important property of the type operand is its type.
                    AllocateExpression(
                        SX_NOTHING,
                        IsType,
                        Input->AsTypeValue()->TargetType->TextSpan),
                    Input->TextSpan);
            break;
        }

        case ParseTree::Expression::CastObject:

            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            __fallthrough;

        case ParseTree::Expression::CastBoolean:
        case ParseTree::Expression::CastCharacter:
        case ParseTree::Expression::CastDate:
        case ParseTree::Expression::CastDouble:
        case ParseTree::Expression::CastSignedByte:
        case ParseTree::Expression::CastByte:
        case ParseTree::Expression::CastShort:
        case ParseTree::Expression::CastUnsignedShort:
        case ParseTree::Expression::CastInteger:
        case ParseTree::Expression::CastUnsignedInteger:
        case ParseTree::Expression::CastLong:
        case ParseTree::Expression::CastUnsignedLong:
        case ParseTree::Expression::CastDecimal:
        case ParseTree::Expression::CastSingle:
        case ParseTree::Expression::CastString:
        {
            Type *TargetType = NULL;

            switch (Input->Opcode)
            {
                case ParseTree::Expression::CastBoolean:
                    TargetType = GetFXSymbolProvider()->GetBooleanType();
                    break;
                case ParseTree::Expression::CastCharacter:
                    TargetType = GetFXSymbolProvider()->GetCharType();
                    break;
                case ParseTree::Expression::CastDate:
                    TargetType = GetFXSymbolProvider()->GetDateType();
                    break;
                case ParseTree::Expression::CastDouble:
                    TargetType = GetFXSymbolProvider()->GetDoubleType();
                    break;
                case ParseTree::Expression::CastSignedByte:
                    TargetType = GetFXSymbolProvider()->GetSignedByteType();
                    break;
                case ParseTree::Expression::CastByte:
                    TargetType = GetFXSymbolProvider()->GetByteType();
                    break;
                case ParseTree::Expression::CastShort:
                    TargetType = GetFXSymbolProvider()->GetShortType();
                    break;
                case ParseTree::Expression::CastUnsignedShort:
                    TargetType = GetFXSymbolProvider()->GetUnsignedShortType();
                    break;
                case ParseTree::Expression::CastInteger:
                    TargetType = GetFXSymbolProvider()->GetIntegerType();
                    break;
                case ParseTree::Expression::CastUnsignedInteger:
                    TargetType = GetFXSymbolProvider()->GetUnsignedIntegerType();
                    break;
                case ParseTree::Expression::CastLong:
                    TargetType = GetFXSymbolProvider()->GetLongType();
                    break;
                case ParseTree::Expression::CastUnsignedLong:
                    TargetType = GetFXSymbolProvider()->GetUnsignedLongType();
                    break;
                case ParseTree::Expression::CastDecimal:
                    TargetType = GetFXSymbolProvider()->GetDecimalType();
                    break;
                case ParseTree::Expression::CastSingle:
                    TargetType = GetFXSymbolProvider()->GetSingleType();
                    break;
                case ParseTree::Expression::CastString:
                    TargetType = GetFXSymbolProvider()->GetStringType();
                    break;
                case ParseTree::Expression::CastObject:
                    TargetType = GetFXSymbolProvider()->GetObjectType();
                    break;
                default:
                    VSFAIL("Surprising conversion opcode.");
            }

            Result =
                InterpretExpressionWithTargetType(
                    Input->AsUnary()->Operand,
                    (Flags & ExprMustBeConstant) | (ExprScalarValueFlags | ExprIsExplicitCast | ExprHasExplicitCastSemantics | ExprForceRValue),
                    TargetType);
            Result->Loc = Input->TextSpan;

            // Useful for type inference to know if the Nothing literal has been typed
            // by itself or with an explicit type cast around it.
            //
            Result->IsExplicitlyCast = true;

            break;
        }

        case ParseTree::Expression::TypeReference:
        {
            if (m_Lookup == NULL)
            {

                // If there is no scope in which to look up names, the compiler is
                // attempting to process simple constant expressions, and needs a cue
                // that this is not a simple constant expression. Actually generating
                // an error message would be incorrect here.
                //
                // 

                return AllocateBadExpression(Input->TextSpan);
            }

            bool TypeIsBad = false;

            Type *ReferencedType =
                InterpretTypeName(
                    Input->AsTypeReference()->ReferencedType,
                    TypeIsBad);

            if (TypeIsBad)
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            GenericBinding  *pGenericBinding = ReferencedType->IsGenericBinding() ? ReferencedType->PGenericBinding() : NULL;
            VSASSERT( NULL == pGenericBinding || Input->AsTypeReference()->ReferencedType->Opcode == ParseTree::Type::Nullable, "Generic binding not generated by NULLABLE");

            Result =
                ReferToSymbol(
                    Input->TextSpan,
                    ReferencedType->PNamedRoot(),
                    chType_NONE,
                    NULL,
                    pGenericBinding,
                    Flags);

            break;
        }

        case ParseTree::Expression::Concatenate:
        case ParseTree::Expression::Plus:
        {
            // Some tools, such as ASP .NET, generate expressions containing thousands
            // of string concatenations. For this reason, for string concatenations,
            // avoid the usual recursion along the left side of the parse. Also, attempt
            // to flatten whole sequences of string literal concatenations to avoid
            // allocating space for intermediate results.

            // Simple concatenations of two constant strings are handled by the normal
            // logic and aren't caught here.

            // To perform the analysis without recursion, we walk the left side of the parse
            // to first count how deep it is then build an expression stack (in the form of
            // an array). We then walk the tree again, interpret each term, and put it in the
            // stack. Finally, we reduce the stack until all operators are consumed.

            // Plus (+) can sometimes be string concatenation. If instead it binds to addition
            // or a user-defined operator, the correct behavior falls out of the algorithm
            // below.

            ExpressionFlags OperandMask = ExprMustBeConstant;
            ExpressionFlags OperandFlags = ExprScalarValueFlags | ExprIsOperandOfConcatenate;

            // Determine how left-deep the parse tree is. The number of terms is the number
            // of left consecutive operators + 1.

            unsigned TermCount = 2;

            ParseTree::BinaryExpression *Current = Input->AsBinary();
            while (Current->Left->Opcode == ParseTree::Expression::Concatenate ||
                   Current->Left->Opcode == ParseTree::Expression::Plus)
            {
                TermCount++;
                Current = Current->Left->AsBinary();
            }

            // If we have a sufficient number of terms, build an expression stack (in the
            // form of an array) and avoid recursion.  Otherwise, just do normal recurison.

            if (TermCount > 2)
            {
                struct Term
                {
                    ParseTree::Expression *Parent;
                    ILTree::Expression *Element;
                };

                NorlsAllocator Scratch(NORLSLOC);

                Term TermsScratch[10];
                Term *Terms = (TermCount > 10) ? new(Scratch) Term[TermCount] : TermsScratch;
                bool AllStringConstants = true;

                // Fill the expression stack bottom-up by walking the parse tree top-down and
                // interpreting each expression along the way. Make a note when we encounter
                // a non-constant string because that stops us from collapsing the whole tree
                // into one constant.

                Current = Input->AsBinary();
                unsigned StackIndex = TermCount - 1;
                size_t ResultLength = 0;
                IDE_CODE(unsigned IDEFlags = 0);

                do
                {
                    ILTree::Expression *BoundTerm =
                        InterpretExpression(
                            Current->Right,
                            (Flags & OperandMask) | OperandFlags);

                    if (BoundTerm->bilop == SX_CNS_STR)
                    {
                        // Check for overflow, Dev10 651213.  Of course, this is only part of the story.
                        // If we pass the overflow check, we may still get an Out of memory when we ultimately ask
                        // for the memory--which we will let fly as per fail-fast policy
                        if ( !VBMath::TryAdd( ResultLength, BoundTerm->AsStringConstant().Length, &ResultLength ))
                        {
                            ReportSemanticError(ERRID_ContantStringTooLong, Input->TextSpan);
                            return AllocateBadExpression(Input->TextSpan);
                        }

                        IDE_CODE(IDEFlags |= BoundTerm->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS);
                    }
                    else
                    {
                        AllStringConstants = false;
                    }

                    Terms[StackIndex].Parent = Current;
                    Terms[StackIndex].Element = BoundTerm;
                    StackIndex--;
                } while ((StackIndex > 0) && (Current = Current->Left->AsBinary()));

                // We've finished all the right operands.  Now do the dangling left operand.

                ILTree::Expression *Left =
                    InterpretExpression(
                        Current->Left,
                        (Flags & OperandMask) | OperandFlags);

                if (Left->bilop == SX_CNS_STR)
                {
                    // Check for overflow, Dev10 651213.  Of course, this is only part of the story.
                    // If we pass the overflow check, we may still get an Out of memory when we ultimately ask
                    // for the memory--which we will let fly as per fail-fast policy.
                    if ( !VBMath::TryAdd( ResultLength, Left->AsStringConstant().Length, &ResultLength ))
                    {
                        ReportSemanticError(ERRID_ContantStringTooLong, Input->TextSpan);
                        return AllocateBadExpression(Input->TextSpan);
                    }

                    IDE_CODE(IDEFlags |= Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS);
                }
                else
                {
                    AllStringConstants = false;
                }

                Terms[0].Parent = Current;
                Terms[0].Element = Left;

                // If all the operands were string constants, we can collapse everything into
                // one constant, avoiding nasty intermediate results.  Don't optimize when
                // dumping xml.

                if (AllStringConstants && !m_IsGeneratingXML)
                {
                    WCHAR *ResultString = new(m_TreeStorage) WCHAR[ResultLength + 1];
                    ResultString[ResultLength] = 0;
                    size_t WrittenLength = 0;

                    for (StackIndex = 0; StackIndex < TermCount; StackIndex++)
                    {
                        memcpy(
                            ResultString + WrittenLength,
                            Terms[StackIndex].Element->AsStringConstant().Spelling,
                            Terms[StackIndex].Element->AsStringConstant().Length * sizeof(WCHAR));

                        WrittenLength += Terms[StackIndex].Element->AsStringConstant().Length;
                    }

                    VSASSERT(WrittenLength == ResultLength, "String literal concatenation confused.");

                    Location Span;

                    return
                        ProduceStringConstantExpression(
                            ResultString,
                            ResultLength,
                            GetSpan(
                                Span,
                                Terms[0].Element->Loc,
                                Terms[TermCount - 1].Element->Loc)
                            IDE_ARG(IDEFlags));
                }

                // Otherwise walk the stack again, reducing as we go. A Left and a Right term
                // get consumed by a binary operator to produce a new Left term.

                for (StackIndex = 1; StackIndex < TermCount; StackIndex++)
                {
                    ParseTree::Expression *Operator = Terms[StackIndex].Parent;
                    ILTree::Expression *Right = Terms[StackIndex].Element;

                    if (IsBad(Left) || IsBad(Right))
                    {
                        Left = AllocateBadExpression(Operator->TextSpan);
                    }
                    else
                    {
                        Left =
                            InterpretBinaryOperation(
                                Operator->Opcode,
                                Operator->TextSpan,
                                Left,
                                Right,
                                Flags);
                    }
                }

                // We've consumed all operands and we have our result.
                Result = Left;
            }
            else
            {
                // Otherwise just do normal recursion.

                ILTree::Expression *Left =
                    InterpretExpression(
                        Input->AsBinary()->Left,
                        (Flags & OperandMask) | OperandFlags);

                ILTree::Expression *Right =
                    InterpretExpression(
                        Input->AsBinary()->Right,
                        (Flags & OperandMask) | OperandFlags);

                if (IsBad(Left) || IsBad(Right))
                {
                    return AllocateBadExpression(Input->TextSpan);
                }

                Result =
                    InterpretBinaryOperation(
                        Input->Opcode,
                        Input->TextSpan,
                        Left,
                        Right,
                        Flags);
            }

            // If we're at the top of a bound concatenate tree, select the optimal overload of System.String.Concat()
            if (!IsBad(Result) &&
                Result->bilop == SX_CONC &&
                Result->vtype == t_string &&  //Note (8/24/2001):  Only optimize for string concatenations.  Object concatenation is complicated by DBNull.
                !HasFlag(Flags, ExprIsOperandOfConcatenate) &&
                !m_IsGeneratingXML)
            {
                Result = OptimizeConcatenate(Result, Result->Loc);
            }

            break;
        }

        // Comparison operators

        case ParseTree::Expression::Like:
        case ParseTree::Expression::Equal:
        case ParseTree::Expression::NotEqual:
        case ParseTree::Expression::LessEqual:
        case ParseTree::Expression::GreaterEqual:
        case ParseTree::Expression::Less:
        case ParseTree::Expression::Greater:

        // Binary operators

        case ParseTree::Expression::Minus:
        case ParseTree::Expression::Multiply:
        case ParseTree::Expression::Power:
        case ParseTree::Expression::Divide:
        case ParseTree::Expression::Modulus:
        case ParseTree::Expression::IntegralDivide:
        case ParseTree::Expression::ShiftLeft:
        case ParseTree::Expression::ShiftRight:
        case ParseTree::Expression::Xor:
        case ParseTree::Expression::Or:
        case ParseTree::Expression::OrElse:
        case ParseTree::Expression::And:
        case ParseTree::Expression::AndAlso:
        {
            ExpressionFlags OperandMask = ExprMustBeConstant;

            if (Input->Opcode == ParseTree::Expression::OrElse ||
                Input->Opcode == ParseTree::Expression::AndAlso)
            {
                OperandMask |= ExprIsOperandOfConditionalBranch;
            }

            ExpressionFlags OperandFlags = ExprScalarValueFlags;

            ILTree::Expression *Left =
                InterpretExpression(
                    Input->AsBinary()->Left,
                    (Flags & OperandMask) | OperandFlags);

            ILTree::Expression *Right =
                InterpretExpression(
                    Input->AsBinary()->Right,
                    (Flags & OperandMask) | OperandFlags);

            if (IsBad(Left) || IsBad(Right))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            Result =
                InterpretBinaryOperation(
                    Input->Opcode,
                    Input->TextSpan,
                    Left,
                    Right,
                    Flags);

            break;
        }

        case ParseTree::Expression::Is:
        case ParseTree::Expression::IsNot:
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            ILTree::Expression *Left =
                InterpretExpression(
                    Input->AsBinary()->Left,
                    ExprForceRValue);

            //
            // "foo.Form1 IS something" is translated to "foo.m_Form1 IS something" when
            // Form1 is a property generated by MyGroupCollection
            // Oherwise 'foo.Form1 IS Nothing" would be always false because 'foo.Form1'
            // property call creates an instance on the fly.
            Left = this->AlterForMyGroup(Left, Input->AsBinary()->Left->TextSpan);

            ILTree::Expression *Right =
                InterpretExpression(
                    Input->AsBinary()->Right,
                    ExprForceRValue);

            // do the same as above on right part for MyGroup properties
            Right = AlterForMyGroup(Right, Input->AsBinary()->Right->TextSpan);

            bool fIsNullable = false;

            if (!IsBad(Left))
            {
                if (TypeHelpers::IsReferenceType(Left->ResultType))
                {
                    Left = ConvertWithErrorChecking(Left, GetFXSymbolProvider()->GetObjectType(), ExprForceRValue);
                }
                else if (TypeHelpers::IsNullableType(Left->ResultType, m_CompilerHost))
                {
                    if (!IsBad(Right) && !IsNothingLiteral(Right))
                    {
                        ReportSemanticError(
                            (Input->Opcode == ParseTree::Expression::IsNot)?
                                ERRID_IsNotOperatorNullable1 :
                                ERRID_IsOperatorNullable1,
                            Left->Loc,
                            Left->ResultType);

                        MakeBad(Left);
                    }
                    fIsNullable = true;
                }
                else if (TypeHelpers::IsGenericParameter(Left->ResultType) && !Left->ResultType->PGenericParam()->IsValueType())
                {
                    if (!IsBad(Right) && !IsNothingLiteral(Right))
                    {
                        ReportSemanticError(
                            (Input->Opcode == ParseTree::Expression::IsNot)?
                                ERRID_IsNotOperatorGenericParam1 :
                                ERRID_IsOperatorGenericParam1,
                            Left->Loc,
                            Left->ResultType);

                        MakeBad(Left);
                    }
                }
                else
                {
                    ReportSemanticError(
                        (Input->Opcode == ParseTree::Expression::IsNot)?
                            ERRID_IsNotOpRequiresReferenceTypes1 :
                            ERRID_IsOperatorRequiresReferenceTypes1,
                        Left->Loc,
                        Left->ResultType);

                    MakeBad(Left);
                }
            }

            if (!IsBad(Right))
            {
                if (TypeHelpers::IsReferenceType(Right->ResultType))
                {
                    Right = ConvertWithErrorChecking(Right, GetFXSymbolProvider()->GetObjectType(), ExprForceRValue);
                }
                else if (TypeHelpers::IsNullableType(Right->ResultType, m_CompilerHost))
                {
                    if (!IsBad(Left) && !IsNothingLiteral(Left))
                    {
                        ReportSemanticError(
                            (Input->Opcode == ParseTree::Expression::IsNot)?
                                ERRID_IsNotOperatorNullable1 :
                                ERRID_IsOperatorNullable1,
                            Right->Loc,
                            Right->ResultType);

                        MakeBad(Right);
                    }
                    fIsNullable = true;
                }
                else if (TypeHelpers::IsGenericParameter(Right->ResultType) && !Right->ResultType->PGenericParam()->IsValueType())
                {
                    if (!IsBad(Left) && !IsNothingLiteral(Left))
                    {
                        ReportSemanticError(
                            ERRID_IsOperatorGenericParam1,
                            Right->Loc,
                            Right->ResultType);

                        MakeBad(Right);
                    }
                }
                else
                {
                    ReportSemanticError(
                        (Input->Opcode == ParseTree::Expression::IsNot)?
                            ERRID_IsNotOpRequiresReferenceTypes1 :
                            ERRID_IsOperatorRequiresReferenceTypes1,
                        Right->Loc,
                        Right->ResultType);

                    MakeBad(Right);
                }
            }

/*
            // ISSUE: we'll probably need these checks for operator Is
            // Equality and inequality are supported on reference types so long as
            // one type is derived from the other.

            if ((Opcode == SX_EQ || Opcode == SX_NE || Opcode == SX_IS))
            {
                if (TypeHelpers::IsClassOrInterfaceType(LeftType) &&
                TypeHelpers::IsClassOrInterfaceType(RightType))
                {
                    if (IsOrInheritsFromOrImplements(LeftType, RightType))
                    {
                        return RightType;
                    }

                    if (IsOrInheritsFromOrImplements(RightType, LeftType))
                    {
                        return LeftType;
                    }
                }

                if (TypeHelpers::IsArrayType(LeftType) && TypeHelpers::IsArrayType(RightType))
                {
                    ArrayType *LeftArray = LeftType->PArrayType();
                    ArrayType *RightArray = RightType->PArrayType();

                    if (LeftArray->GetRank() == RightArray->GetRank())
                    {
                        Type *LeftElementType = TypeHelpers::GetElementType(LeftArray);
                        Type *RightElementType = TypeHelpers::GetElementType(RightArray);

                        if (IsOrInheritsFromOrImplements(LeftElementType, RightElementType))
                        {
                            return RightType;
                        }

                        if (IsOrInheritsFromOrImplements(RightElementType, LeftElementType))
                        {
                            return LeftType;
                        }
                    }
                }
            }
*/

            if (IsBad(Left) || IsBad(Right))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            // If any of the left or right operands of the Is or IsNot operands
            // are entities of type parameters types, then they need to be boxed.
            //
            if (TypeHelpers::IsGenericParameter(Left->ResultType))
            {
                Left = Convert(Left, GetFXSymbolProvider()->GetObjectType(), ExprNoFlags, ConversionWidening);
            }

            if (TypeHelpers::IsGenericParameter(Right->ResultType))
            {
                Right = Convert(Right, GetFXSymbolProvider()->GetObjectType(), ExprNoFlags, ConversionWidening);
            }

            Result =
                AllocateExpression(
                    (ParseTree::Expression::Is == Input->Opcode) ? SX_IS : SX_ISNOT,
                    GetFXSymbolProvider()->GetBooleanType(),
                    Left,
                    Right,
                    Input->TextSpan);

            if (fIsNullable)
            {
                SetFlag32(Result, SXF_OP_LIFTED_NULLABLE);
            }

            break;
        }

        // Unary operators
        case ParseTree::Expression::Await:
        {
            Result = InterpretAwaitExpression(Input->TextSpan, Input->AsUnary()->Operand, Flags);
            break;
        }

        case ParseTree::Expression::Negate:
        case ParseTree::Expression::Not:
        case ParseTree::Expression::UnaryPlus:
        {
            ExpressionFlags OperandMask = ExprMustBeConstant;

            ILTree::Expression *Operand =
                InterpretExpression(
                    Input->AsUnary()->Operand,
                    (Flags & OperandMask) | ExprScalarValueFlags);

            if (IsBad(Operand))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            Result = InterpretUnaryOperation(Input->Opcode, Input->TextSpan, Operand, Flags);
            break;
        }

        case ParseTree::Expression::AddressOf:
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            ILTree::Expression *Operand =
                InterpretExpression
                (
                    Input->AsUnary()->Operand,
                    ExprIsExplicitCallTarget |
                    // This is needed to support late bind relaxation. If we don't pass this flag the late call
                    // will be interpreted already and that will always ensure it is a SXE_LATE_GET
                    // If we place this in a lambda sub we will have the problem that an entry gets left on the stack
                    // If we place this is a sub we need to esure it is a SXE_LATE_CALL such that the runtime
                    // will drop the argument. This will happen by flipping the interal bits for this.
                    ExprPropagatePropertyReference
                );

            if (!Operand || IsBad(Operand))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            if
            (
                (
                    Operand->bilop != SX_SYM ||
                    !IsProcedure(Operand->AsSymbolReferenceExpression().Symbol) ||
                    IsEvent(Operand->AsSymbolReferenceExpression().Symbol)
                ) &&
                Operand->bilop != SX_OVERLOADED_GENERIC &&
                Operand->bilop != SX_EXTENSION_CALL &&
                !IsLateReference(Operand)
            )
            {
                ReportSemanticError(
                    ERRID_AddressOfOperandNotMethod,
                    Operand->Loc);

                return AllocateBadExpression(Input->TextSpan);
            }

            // Partial methods declarations (without an implementation) are not allowed
            // to be used in AddressOf.

            if( Operand->bilop == SX_SYM && IsProcedure( Operand->AsSymbolReferenceExpression().Symbol ) )
            {
                Procedure* p = ViewAsProcedure( Operand->AsSymbolReferenceExpression().Symbol );
                if( p->IsPartialMethodDeclaration() )
                {
                    ReportSemanticError(
                        ERRID_NoPartialMethodInAddressOf1,
                        Operand->Loc,
                        Operand->AsSymbolReferenceExpression().Symbol
                        );

                    return AllocateBadExpression( Input->TextSpan );
                }
            }

            // The final disposition of the AddressOf (delegate binding or
            // taking the address of a shared method) depends on type context information
            // that is not available at this point. The SX_ADDRESSOF node serves as
            // a placeholder and will be consumed when the target context becomes
            // available.

            Result =
                AllocateExpression(
                    SX_ADDRESSOF,
                    TypeHelpers::GetVoidType(),
                    Operand,
                    Input->TextSpan);

            if (m_DisallowMeReferenceInConstructorCall)
            {
                // Can't emit an error here, so propagate this state information through the
                // trees so we can emit an error after the AddressOf expression is fully analyzed.

                SetFlag32(Result, SXF_DISALLOW_ME_REFERENCE);
            }

            if (Input->AsUnary()->Operand->Opcode == ParseTree::Expression::AlreadyBoundSymbol)
            {
                SetFlag32(Result, SXF_TARGET_METHOD_RESOLVED);
            }

            if (Input->AsAddressOf()->UseLocationOfTargetMethodForStrict)
            {
                SetFlag32(Result, SXF_USE_STRICT_OF_TARGET_METHOD);
            }
            break;
        }

        case ParseTree::Expression::Conversion:
        case ParseTree::Expression::DirectCast:
        case ParseTree::Expression::TryCast:
        {
            // Get the type to cast to.
            bool TypeIsBad = false;
            Type *TargetType =
                InterpretTypeName(Input->AsTypeValue()->TargetType, TypeIsBad);

            // Next process the expression to cast.
            // Don't infer the result type as we are going to call ConvertWithErrorchecking which will do it once.
            ILTree::Expression *ExpressionToCast =
                InterpretExpression(Input->AsTypeValue()->Value, (Flags & ExprMustBeConstant) | ExprForceRValue | ExprDontInferResultType, 0, NULL, TargetType);

            if (IsBad(ExpressionToCast) || TypeIsBad)
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            ExpressionFlags ConvertFlags = (Flags & ExprMustBeConstant) | (ExprIsExplicitCast | ExprHasExplicitCastSemantics) | ExprGetLambdaReturnTypeFromDelegate;

            if (Input->Opcode == ParseTree::Expression::DirectCast)
            {
                ConvertFlags |= ExprHasDirectCastSemantics;
            }
            else if (Input->Opcode == ParseTree::Expression::TryCast)
            {
                if (TypeHelpers::IsValueType(TargetType))
                {
                    ReportSemanticError(
                        ERRID_TryCastOfValueType1,
                        Input->AsTypeValue()->TargetType->TextSpan,
                        TargetType);

                    return AllocateBadExpression(Input->TextSpan);
                }

                if (TypeHelpers::IsGenericParameter(TargetType) && !TargetType->PGenericParam()->IsReferenceType())
                {
                    ReportSemanticError(
                        ERRID_TryCastOfUnconstrainedTypeParam1,
                        Input->AsTypeValue()->TargetType->TextSpan,
                        TargetType);

                    return AllocateBadExpression(Input->TextSpan);
                }

                ConvertFlags |= ExprHasTryCastSemantics;
            }

            Result = ConvertWithErrorChecking(ExpressionToCast, TargetType, ConvertFlags);

            // UI consumers need the full text span, including the type name
            // same for an unoptimized CType, TryCast, DirectCast  ( note: CType(x, TypeOfx) get optimized to 'x')
            // see bugs 456711, 57980
            if (m_IsGeneratingXML || Result != ExpressionToCast)
            {
                Result->Loc = Input->TextSpan;
            }

            if (IsBad(Result))
            {
                return AllocateBadExpression(Input->TextSpan);
            }

            // Useful for type inference to know if the Nothing literal has been typed
            // by itself or with an explicit type cast around it.
            //
            Result->IsExplicitlyCast = true;

            break;
        }

        case ParseTree::Expression::IIf:
            Result = InterpretIIF(Input->AsIIf(), Flags);
            break;

        case ParseTree::Expression::GetType:
            Result = InterpretGetType(Input->AsGetType(), Flags);
            break;

        case ParseTree::Expression::GetXmlNamespace:
            Result = InterpretGetXmlNamespace(Input->AsGetXmlNamespace(), Flags);
            break;

        case ParseTree::Expression::From:
        case ParseTree::Expression::CrossJoin:
        case ParseTree::Expression::Where:
        case ParseTree::Expression::GroupBy:
        case ParseTree::Expression::Aggregate:
        case ParseTree::Expression::Select:
        case ParseTree::Expression::OrderBy:
        case ParseTree::Expression::Distinct:
        case ParseTree::Expression::InnerJoin:
        case ParseTree::Expression::GroupJoin:
        case ParseTree::Expression::Take:
        case ParseTree::Expression::Skip:
        case ParseTree::Expression::TakeWhile:
        case ParseTree::Expression::SkipWhile:

            if (HasFlag(Flags, ExprMustBeConstant) || m_InConstantExpressionContext)
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    Input->TextSpan);

                return AllocateBadExpression(Input->TextSpan);
            }

            Result = InterpretLinqQuery(Input, Flags);
            break;

        case  ParseTree::Expression::Equals:    // join predicate : note: if you get here, you probably bypassed the query operators. see Semantics::LambdaBodyBuildKeyExpressions::BuildKey
        case  ParseTree::Expression::LinqSource:
        case  ParseTree::Expression::Let:
        case  ParseTree::Expression::GroupRef:
            // shouldn't come here
            ReportSemanticError(
                ERRID_InternalCompilerError,
                Input->TextSpan); // shouldn't come here
            Result = AllocateBadExpression(Input->TextSpan);// shouldn't come here
            break;

        case ParseTree::Expression::ImplicitConversion:
            {
                ParseTree::ImplicitConversionExpression * conv = Input->AsImplicitConversion();
                Result = InterpretExpressionWithTargetType(conv->Value, Flags | ExprForceRValue, conv->TargetType);
            }
            break;

        case  ParseTree::Expression::QueryOperatorCall:
            Result = InterpretQueryOperatorCall(Input->AsQueryOperatorCall()->operatorCall, Flags | ExprForceRValue);
            break;

        case  ParseTree::Expression::QueryAggregateGroup:
            Result = InterpretGroupForAggregateExpression(Input->AsQueryAggregateGroup(), Flags);
            break;

        case ParseTree::Expression::Lambda:
            {
                // Dev10 #521739 Don't even try to continue if we are in context of a constant expression.
                if (HasFlag(Flags, ExprMustBeConstant) || m_InConstantExpressionContext)
                {
                    ReportSemanticError(
                        ERRID_RequiredConstExpr,
                        Input->TextSpan);

                    return AllocateBadExpression(Input->TextSpan);
                }

                ExpressionFlags LambdaBodyFlags = Flags;
                if (m_DisallowMeReferenceInConstructorCall)
                {
                    SetFlag(LambdaBodyFlags, ExprIsInitializationCall);
                }

                // 




                // The body of a lambda should be forced as an RValue. This is to not allow
                // lambda's to be associated with void delegates.
                LambdaBodyFlags = (Input->AsLambda()->MethodFlags & DECLF_Function) ?  
                                  (LambdaBodyFlags  | ExprForceRValue) : 
                                  (LambdaBodyFlags & ~ExprForceRValue);

                Result = InterpretLambdaExpression(Input->AsLambda(), LambdaBodyFlags);
            }
            break;

        // XmlCharData, XmlReference handled the same as StringLiteral
        // They are left in the syntax tree after processing so they need to be handled here as well.

        case ParseTree::Expression::XmlElement:
        case ParseTree::Expression::XmlAttribute:
        case ParseTree::Expression::XmlName:
        case ParseTree::Expression::XmlPI:
        case ParseTree::Expression::XmlDocument:
        case ParseTree::Expression::XmlComment:
        case ParseTree::Expression::XmlCData:
        case ParseTree::Expression::XmlAttributeValueList:
        case ParseTree::Expression::XmlEmbedded:
            Result = InterpretXmlExpression(Input, Flags);
            break;

        case ParseTree::Expression::ArrayInitializer:
            Result = InterpretArrayLiteral(Input->AsArrayInitializer(), Flags & ~ExprAccessDefaultProperty);
            break;
        case ParseTree::Expression::CollectionInitializer:
            Result = InterpretCollectionInitializer(Input->AsCollectionInitializer(), Flags);
            break;
        default:

            VSFAIL("Surprising expression opcode.");
            return AllocateBadExpression(Input->TextSpan);
    }

    VSASSERT(Result != NULL, "The expression was not interpreted correctly. This will crash later");

    Result = ApplyContextSpecificSemantics(Result, Flags, TargetType);

    if (!HasFlag(Flags, ExprDontInferResultType))
    {
        // In case we need to infer for an unbound lambda, generate an anonymous type
        // first so that the lambda can be converted to an addressOf and that to a delegate of this type in
        // InterpretInitializer.
        // Do not do this if there are errors when interpreting because we will report them twice otherwise.
        if (Result->bilop == SX_UNBOUND_LAMBDA)
        {
            // 




            unsigned CurrentNumberOfErrors = m_Errors ? m_Errors->GetErrorCount() : 0;
            if (CurrentNumberOfErrors > NumberOfErrors)
            {
                MakeBad(Result);
            }
            else
            {
                BackupValue<TriState<bool>> backup_report_type_inference_errors(&m_ReportMultilineLambdaReturnTypeInferenceErrors);
                bool ranDominantTypeAlgorithm = false;
                
                Type* TargetType = InferLambdaType(&Result->AsUnboundLambdaExpression(), Result->Loc, &ranDominantTypeAlgorithm);

                // This only applies to Multiline Function Lambdas:
                // If  the lambda is of the form:
                // Dim x = Function()
                //             ...
                //         End Function 
                // The Delegate return type was generated by using the dominant type algorithm on the 
                // lambda body. We do not want to call the dominant type algorithm again when we are looking for
                // a target type to interpret the lambda with, so we set the following flag.
                Flags = Flags | ExprGetLambdaReturnTypeFromDelegate;
                
                if (TargetType && !IsBad(Result))
                {
                    // When we call ConvertWithErrorChecking, we are compiling the lambda. If we ran the dominant type algorithm while inferring the
                    // lambda type, then we have already interpretted the lambda block and reported any type inference errors we could; thus, we do not
                    // want to report these errors twice. However, if we did not run the dominant type algorithm, we want to pass the current value of 
                    // m_ReportMultilineLambdaReturnTypeInferenceErrors through because we may come across type inference errors we haven't reported yet.
                    // See InferMultilineLambdaReturnTypeFromReturnStatements() for more details.
                    if (m_ReportMultilineLambdaReturnTypeInferenceErrors.HasValue())
                    {
                        m_ReportMultilineLambdaReturnTypeInferenceErrors.SetValue( 
                            !ranDominantTypeAlgorithm  ? 
                            m_ReportMultilineLambdaReturnTypeInferenceErrors.GetValue() : 
                            false);
                    }
                    
                    Result = ConvertWithErrorChecking(Result, TargetType, Flags);
                }

            }
        }        
    }

    return Result;
}

bool Semantics::ValidateShape
(
    ParseTree::ArrayInitializerExpression * pInput,
    ArrayList<unsigned> & lengthList
)
{
    bool ret = ValidateShape(pInput, lengthList, 0, true);

    return ret;
}

bool Semantics::ValidateShape
(
    ParseTree::ArrayInitializerExpression * pInput,
    ArrayList<unsigned> & lengthList,
    unsigned dimIndex,
    bool first
)
{
    bool ret = true;
        
    ThrowIfNull(pInput);
    ThrowIfNull(pInput->Elements);
    
    while ((first) && lengthList.Count() <= dimIndex)
    {
        lengthList.Add(0);
    }

    const Location & errorLocation = pInput->Elements->InitialValues ? pInput->Elements->InitialValues->TextSpan : pInput->Elements->TextSpan;
    
    if (lengthList.Count() <= dimIndex)
    {
        ReportSemanticError(ERRID_ArrayInitializerTooManyDimensions, errorLocation);
        return false;
    }

    int count = GetElementCount(pInput);
    
    if (first)
    {
        lengthList[dimIndex] = count;
    }
    else
    {
        ret = ValidateElementCount(lengthList[dimIndex], count, errorLocation);
    }

    if (ret && pInput->Elements)
    {
        ParseTree::InitializerList *  pInitValues = pInput->Elements->InitialValues;

        bool nestedFirst = first;
        
        while (pInitValues)
        {
            ParseTree::Expression * pExpression = GetInitializerValue(pInitValues->Element);

            if (pExpression->Opcode == ParseTree::Expression::ArrayInitializer)
            {
                ret = ret && ValidateShape((ParseTree::ArrayInitializerExpression *)pExpression, lengthList, dimIndex + 1, nestedFirst);
            }
            else
            {
                if (dimIndex != lengthList.Count() - 1)
                {
                    ReportSemanticError(ERRID_ArrayInitializerTooFewDimensions, pExpression->TextSpan);
                }
            }
            nestedFirst = false;
            pInitValues = pInitValues->Next;
        }
    }

    return ret;
}


unsigned Semantics::GetElementCount
(
    ParseTree::ArrayInitializerExpression * pExpr
)
{
    ThrowIfNull(pExpr);
    ThrowIfNull(pExpr->Elements);
    ParseTree::InitializerList * pElements = pExpr->Elements->InitialValues;

    unsigned count = 0;
    
    while (pElements)
    {
        ++count;
        pElements = pElements->Next;
    }

    return count;
}

ParseTree::Expression * 
Semantics::GetInitializerValue
(
    ParseTree::Initializer * pInitializer
)
{
    ThrowIfNull(pInitializer);

    switch (pInitializer->Opcode)
    {
        case ParseTree::Initializer::Expression:
            return ((ParseTree::ExpressionInitializer *)pInitializer)->Value;
        case ParseTree::Initializer::Deferred:
            return GetInitializerValue(((ParseTree::DeferredInitializer *)pInitializer)->Value);
        case ParseTree::Initializer::Assignment:
            return GetInitializerValue(((ParseTree::AssignmentInitializer *)pInitializer)->Initializer);
        default:
            Assume(false, L"Unexpected initializer detected.");
            return NULL;
    }
}

bool Semantics::ValidateElementCount
(
    unsigned expectedCount,
    unsigned count,
    const Location & location
)
{
    if (expectedCount == count)
    {
        return true;
    }

    StringBuffer difference;
    WCHAR differenceSpelling[20];

    if
    (
        SUCCEEDED
        (
            StringCchPrintf(
                differenceSpelling,
                DIM(differenceSpelling),
                L"%d",
                count > expectedCount?
                    count - expectedCount :
                    expectedCount - count
            )
        )
    )
    {
        difference.AppendString(differenceSpelling);
    }

    unsigned errorID = 0;
    
    if (expectedCount > count)
    {
        errorID = ERRID_InitializerTooFewElements1;
    }
    else
    {
        ThrowIfFalse(expectedCount < count);
        errorID = ERRID_InitializerTooManyElements1;
    }    

    ReportSemanticError(errorID, location, difference);
    return false;
}

ParseTree::ArgumentList *
Semantics::TranslateCollectionInitializerElement
(
    ParserHelper & ph, 
    ParseTree::Expression * pValue
)
{
    ThrowIfNull(pValue);

    ParseTree::ArgumentList * pRet = NULL;
    ParseTree::ArgumentList * pCurrent = NULL;
        
    if (pValue->Opcode == ParseTree::Expression::ArrayInitializer)
    {
        ParseTree::ArrayInitializerExpression * pNestedInit = pValue->AsArrayInitializer();

        if (pNestedInit && pNestedInit->Elements && pNestedInit->Elements->InitialValues)
        {
            ParseTree::InitializerList * pNestedList = pNestedInit->Elements->InitialValues;

            while (pNestedList)
            {
                if (pCurrent)
                {
                    pCurrent = ph.AddArgument(pCurrent, GetInitializerValue(pNestedList->Element));
                }
                else
                {
                    pRet = pCurrent = ph.CreateArgList(GetInitializerValue(pNestedList->Element));
                }
                pNestedList = pNestedList->Next;
            }
        }
    }
    else
    {
        pRet = ph.CreateArgList(pValue);
    }    

    return pRet;
}

ILTree::Expression *
Semantics::InterpretCollectionInitializerElement
(
    ParserHelper & ph,
    BCSYM_Variable * pCollectionTemporary,
    ParseTree::ArgumentList * pArgs,
    ExpressionFlags flags
)
{
    // This function will construct and interpret the expression "Collection.Add(args)"...

    // "Collection" ...
    ParseTree::AlreadyBoundExpression *receiver = ph.CreateBoundExpression(ReferToSymbol(
                                        pArgs->TextSpan,
                                        pCollectionTemporary, 
                                        chType_NONE,  
                                        NULL, //no base reference
                                        NULL, //no generic binidng
                                        ExprNoFlags ));

    // "Collection.Add" ...
    ParseTree::Expression *add =  ph.CreateQualifiedExpression(
                                        receiver,
                                        ph.CreateNameExpression(STRING_CONST(m_Compiler, Add)),
                                        pArgs->TextSpan,
                                        ParseTree::Expression::DotQualified);


    // "Collection.Add(args)" ...
    ParseTree::CallOrIndexExpression *call = ph.CreateMethodCall(
                                        add,
                                        pArgs,
                                        pArgs->TextSpan);

    // ... and interpret it!
    ILTree::Expression *pRet = InterpretExpression(
                                        call,
                                        (flags & ~ExprForceRValue) | ExprCreateColInitElement | ExprResultNotNeeded);
    
    // What we get back is a SX_COLINITELEMENT with .CallExpression=SX_CALL.
    // If the interpretation was bad, then we get back a bad SX_COLINITELEMENT with a bad .CallExpression.

    return pRet;
}

ILTree::ColInitElementExpression *
Semantics::AllocateColInitElement
(
    ILTree::Expression * pCallExpression, 
    ILTree::Expression * pCopyOutArguments, 
    ExpressionFlags flags,
    const Location & callLocation
)
{
    ILTree::ColInitElementExpression * pRet =
        (ILTree::ColInitElementExpression *)
        AllocateExpression
        (
            SX_COLINITELEMENT,
            TypeHelpers::GetVoidType(),
            callLocation
        );

    pRet->CallExpression = pCallExpression;
    pRet->CallInterpretationFlags = flags;
    pRet->CopyOutArguments = pCopyOutArguments;
    pRet->vtype = t_void;

    if (pCallExpression)
    {
        pRet->uFlags |= SF_INHERIT(pCallExpression->uFlags);
    }
    if (pCopyOutArguments)
    {
        pRet->uFlags |= SF_INHERIT(pCopyOutArguments->uFlags);
    }

    return pRet;
}

ILTree::ColInitExpression *
Semantics::AllocateColInitExpression
(
    ILTree::Expression * pNewExpression, 
    ILTree::ExpressionWithChildren * pElements, 
    BCSYM_Variable * pTmpVar,
    const Location & loc
)
{
    ThrowIfNull(pNewExpression);
    ThrowIfFalse(!pTmpVar || pElements);
    ThrowIfFalse(!pElements || pTmpVar);
    
    ILTree::ColInitExpression * pRet =
        (ILTree::ColInitExpression *)
        AllocateExpression
        (
            SX_COLINIT,
            pNewExpression->ResultType,
            loc
        );

    pRet->Elements = pElements;
    pRet->NewExpression = pNewExpression;
    pRet->ResultTemporary = 
        pTmpVar ? 
            AllocateSymbolReference
            (
                pTmpVar, 
                pTmpVar->GetType(), 
                NULL, 
                loc, 
                NULL
            ) :
        NULL;    

    if (pRet->Elements)
    {
        pRet->uFlags |= SF_INHERIT(pRet->Elements->uFlags);
    }

    pRet->uFlags |= SF_INHERIT(pRet->NewExpression->uFlags);

    if (pRet->ResultTemporary)
    {
        pRet->uFlags |= SF_INHERIT(pRet->ResultTemporary->uFlags    );
    }


    return pRet;
        
}

ILTree::Expression *
Semantics::InterpretCollectionInitializer
(
    ParseTree::CollectionInitializerExpression * pInput, 
    ExpressionFlags flags
)
{
    ThrowIfNull(pInput);
    ILTree::Expression * pNewExpression = InterpretExpression(pInput->NewExpression, flags);
    if (IsBad(pNewExpression))
    {
        return pNewExpression;
    }

    // The user has provided a list e.g. {{test,2},3}. This list has two top-level elements: {test,2} and 3.
    // For any braced expressions at the top level, we treat them as tuples to be passed to
    // a polyadic Add method, rather than as array literals. Thus, the above two elements
    // become Add(test,2) and Add(3). They do not become Add({test,2}) and Add(3).
    // But for nested braced expressions, we don't treat them as tuples. Thus, {{test},2},3}
    // becomes Add({test},2) and Add(3).
    // 
    // If the collection has a single Add method "Add(ByVal x as int())" then our above rule will
    // be confusing, since "new Collection from { {1,2},{3} } would become Add(1,2), Add(3), both
    // of which will fail. Indeed, { {1},{2},{3} } is identical to {1,2,3}! The only way the
    // user can pass arrays to his add function is with parentheses, "from { ({1}), ({2}), ({3}) }".
    // This convention mirrors our convention for array literals, that nested braces always
    // are considered to be higher-rank arrays rather than jagged arrays.
    //
    // There's one odditity. You'd have thought that { {}, {} } would succesfully invoke an argument-less
    // Add method. But we specifically check for this case and disallow it. (Microsoft: I don't understand why?)
    //
    // We would like to give informative messages to the user if they get it wrong.
    // For every top-level element which fails to match in arity to any of the available Add methods,
    // we wish to squiggly that element and say "No overload of Collection.Add takes argument (element).
    // Please rewrite in the form {key,value} / {stuff1,stuff2,stuff3}." i.e. giving parameter-name
    // lists for every available overload of Add.
    // But if the top-level element does match in arity to at least one of the available Add methods,
    // then we won't give our own error. Instead we'll generate code to do Add(...), and we'll leave
    // it to the rest of the compiler to decide if this method invocation is valid or not, and if
    // its arguments can be converted, and which overload to use.
    // 
    // In general, method resolution is complicated. We want to leave it as "black box" as possible.
    // So, this function merely checks for the existence of at least one function named Add. If it
    // finds it, then it goes ahead and generates code to invoke Collection.Add(...). Appropriate
    // error reporting is left to InterpretCallExpression and MatchArguments. They know to report
    // collection-initializer-specific errors thanks to the ExprCreateColInitElement flag.


    Location initErrorLoc;
    ParseTreeHelpers::GetPunctuatorLocation(&pInput->TextSpan, &pInput->From, &initErrorLoc);
    initErrorLoc.SetEnd(&pInput->TextSpan);           
    

    // Verification of collection type: first we ensure that the collection type really is a collection, i.e.
    // basically that once we've created the collection then people will be able to "for each" over it.
    // Microsoft: WHY? All we're doing is invoking the "Add" method on something. Why even care
    // that people can enumerate it afterwards? Was this intended to support "new Collection from MyEnumerable"?
    // But here we're checking the collection pNewExpression, rather than "From" expression pInput->Initializer...
    // Answer: that's what the decision was when the spec was designed. The motive was that this "From"
    // is really intended to be used for collections, and not just for arbitrary classes that happen
    // to have an "Add" method.

    // Spec $10.9 (this comment is in answer to 






    if (MatchesForEachCollectionDesignPattern(pNewExpression, NULL))
    {
        // test (1): if the collection type matches the design pattern, then we're okay
    }

    else if (GetFXSymbolProvider()->GetType(FX::IEnumerableType) &&
             TypeHelpers::IsOrInheritsFromOrImplements(
                                pNewExpression->ResultType,
                                GetFXSymbolProvider()->GetType(FX::IEnumerableType),
                                m_SymbolCreator,
                                false,
                                NULL,
                                m_CompilerHost))
    {
        // tests (2&3): if the collection type inherits from IEnumerable then we're okay
    }

    else
    {
        ReportSemanticError(ERRID_NotACollection1, initErrorLoc, pNewExpression->ResultType);
        return AllocateBadExpression(pNewExpression->ResultType, pInput->TextSpan);
    }



    // Verification of collection type: next, we check that the proposed collection type has at
    // least one "Add" member, and that it will be visible and bindable. We don't yet bother with
    // arities or anything like that. The way we make the check is by attempting to interpret
    // the symbol "(CType(Nothing,CollectionType)).Add" without regard to arities or anything
    // like that. It's a shortcut way of bailing if there's not even a single candidate Add.
    //
    ParserHelper ph(&m_TreeStorage, pInput->TextSpan);
    BackupValue<bool> backup_report_errors(&m_ReportErrors);
    m_ReportErrors = false;
    //
    ILTree::Expression * pExpr =
        InterpretExpression
        (
            ph.CreateQualifiedExpression
            (
                ph.CreateBoundExpression
                (   
                    AllocateExpression(SX_NOTHING, pNewExpression->ResultType, pInput->TextSpan)
                ),
                ph.CreateNameExpression(STRING_CONST(m_Compiler, Add)),
                ParseTree::Expression::DotQualified
            ),
            ExprIsExplicitCallTarget
        );
    backup_report_errors.Restore();

    // So we attempted to bind to the symbol "Add". Did we get something acceptable?
    bool accessible = true; // suppose we did, but set "accessible=false" if proved wrong...

    if  (!pExpr || IsBad(pExpr) || (pExpr->bilop != SX_SYM && pExpr->bilop != SX_EXTENSION_CALL))
    {
        accessible = false;
    }
    else if (pExpr->bilop == SX_SYM &&
             (!pExpr->AsSymbolReferenceExpression().Symbol ||
              !CanBeAccessible(pExpr->AsSymbolReferenceExpression().Symbol, NULL, ContainingClass())))
    {
        VSFAIL("unexpected: how can InterpretExpression have bound to a non-existent or non-accessible member?");
        accessible = false;
    }
    else if (pExpr->bilop == SX_EXTENSION_CALL &&
             (!pExpr->AsExtensionCallExpression().ExtensionCallLookupResult ||
              !CanBeAccessible(pExpr->AsExtensionCallExpression().ExtensionCallLookupResult, NULL, ContainingClass())))
    {
        VSFAIL("unexpected: SX_EXTENSION_CALL expressions only get created if there's at least one accessible extension method defined on the receiver that's accessible from the current context");
        accessible = false;
    }

    if (!accessible)
    {
        ReportSemanticError(ERRID_NoAddMethod1, initErrorLoc, pNewExpression->ResultType);
        return AllocateBadExpression(pNewExpression->ResultType, pInput->TextSpan);
    }



    // Now we accept that the collection-type that the user asked for (be it Dictionary(Of String,Int) or
    // List(Of Frudge) or whatever, is broadly acceptable. We're going to generate and interpret Add
    // calls now. Maybe some of them might fail to bind correctly, if they had types or arities which
    // didn't work with any available Add method/extension-method. But since we know that there is at
    // least one Add method, the error message we give in this case will at least make sense.

    ExpressionListHelper elements(this);

    BCSYM_Variable * pTmpVar = NULL;
    
    if (pInput->Initializer && pInput->Initializer->InitialValues)
    {
        ParseTree::InitializerList * pList = pInput->Initializer->InitialValues;
        pTmpVar = AllocateShortLivedTemporary(pNewExpression->ResultType, &pInput->TextSpan);
        
        for (; pList!=NULL; pList = pList->Next)
        {                
            ParseTree::Expression * pValue = GetInitializerValue(pList->Element);
            ParserHelper ph(&m_TreeStorage, pValue->TextSpan);
            ParseTree::ArgumentList * pArgs = TranslateCollectionInitializerElement(ph, pValue);
            // pArgs: for a top-level element 's', pArgs is now ("s").
            // For a top-level element '{"test",1}', pArgs is now ({"test"},1).

            if (!pArgs)
            {
                ReportSemanticError(ERRID_EmptyAggregateInitializer, pValue->TextSpan);
                elements.Add(AllocateBadExpression(pValue->TextSpan), pValue->TextSpan);
                MakeBad(elements.Start()); 
                continue;
            }

            // And here's where we generate the call to Add(pArgs)...
            ILTree::Expression * pExpr = InterpretCollectionInitializerElement(ph, pTmpVar, pArgs, flags);
            elements.Add(pExpr, pArgs->TextSpan);

            if (IsBad(pExpr))
            {
                MakeBad(elements.Start());
            }
            
        }    
    }

    return AllocateColInitExpression(pNewExpression, elements.Start(), pTmpVar, pInput->TextSpan);
}


ILTree::ExpressionWithChildren *
Semantics::CreateDimList    
(
    unsigned rank,
    unsigned * pDims,
    const Location & loc
)
{
    ThrowIfFalse(rank == 0 || pDims);
    
    ExpressionListHelper dimList(this);
    
    for (unsigned i = 0; i < rank; ++i)
    {
        dimList.Add
        (
            ProduceConstantExpression
            (
                pDims[i],
                loc,
                GetFXSymbolProvider()->GetIntegerType()
                IDE_ARG(0)
            ),
            loc
        );
    }

    return dimList.Start();
}
    

void
Semantics::CheckLambdaParameterShadowing
(
    ILTree::UnboundLambdaExpression * pLambda
)
{
    if (pLambda)
    {
        BCSYM_Param * pParam = pLambda->FirstParameter;

        while (pParam)
        {
            Location * pParamLoc = pParam->GetLocation();
            
            if 
            (!
                CheckNameForShadowingOfLocals
                (
                    pParam->GetName(),
                    pParamLoc ? *pParamLoc : pLambda->Loc,
                    ERRID_LambdaParamShadowLocal1,
                    false
                )
            )
            {
                MakeBad(pLambda);
            }

            pParam = pParam->GetNext();
        }
    }
}



ILTree::Expression * 
Semantics::InterpretArrayLiteral
(
    ParseTree::ArrayInitializerExpression * pInput, 
    ExpressionFlags flags
)
{
    // This function returns an array literal expression and a note of its inferred dominant type.
    // We trust that someone will convert it in the future into a real array, using ConvertArrayLiteral.
    // They might ConvertArrayLiteral(pLiteral,TargetType) to convert it to some target type,
    // or they might ConvertArrayLiteral(pLiteral,NULL) to pick up the dominant type here
    // (and if necessary report warnings/errors on the dominant type).
    //
    // This is what the result looks like:
    //
    // return:ILTree_ArrayLiteralExpression
    //          with ResultType:BCSYM_ArrayLiteralType = a type symbol for the literal
    //                   with type = the dominant type of the list
    //                   with elements = a flattened list of every element in the original array literal, including nested ones
    //          with ElementList = the as-yet-unconverted expressions.
    //
    // If we return an array with nested arrays inside, e.g. { {1,2}, {3,4} }, then "ElementList" will
    // have two IL children: "{1,2}" and "{3,4}". But the return's BCSYM ResultType will contain the
    // flattened list [1,2,3,4]. And the two IL children will have just Void as their own BCSYM ResultType.
    // The intention is that the ClassifyConversion routines can ignore totally the nested structure, and work only
    // with the flattened version.
    //
    // Questions of how to infer the dominant type, and what the flattened list should look like, are in
    // general more complicated. Read the comments in InferDominantTypeOfExpressions to see how it's done.
    //
    // The act of converting an array literal may produce errors, e.g. "Dim x = {function(x)x+1}" produces
    // an error that the type of x isn't yet known. It's okay if "m_ReportErrors==false" to try to
    // convert (e.g. during type inference). But otherwise, we must convert exactly once.
    // Thus: if ever there is an SX_ARRAYLITERAL with type BCSYM_ArrayLiteralType, then it must be converted
    // to something else (usually an SX_ARRAYLITERAL with a BCSYM_ArrayType).
    //
    // Note that when ClassifyConversion is called, it doesn't look at the ResultType of an array literal;
    // instead it looks at each of the ResultType.elements to see if they can be converted.
    //
    //
    // As for the type of an array literal, the BCSYM_ArrayLiteralType is a complicated thing...
    // (1) It stores the DOMINANT TYPE computed for that array literal. If the array literal
    //     is reclassified to its dominant type array, by calling ConvertArrayLiteral(TargetType=NULL),
    //     it will end up as just an array of this type.
    //     e.g. "{1,2}" is an array literal, but "({1,2})" has been classified to just an array
    //     and can never be reclassified.
    // (2) It stores a list of expressions for all the elements that appear in the array literal.
    //     e.g. "{1S,2S}" stores the expressions "1S" and "2S" and has dominant type Short(),
    //     but if someone wanted to reclassify the expression to Long() then it'd work,
    //     by calling ConvertArrayLiteral(pLiteral, TargetType=Long()).
    //     It might seem odd to store information about reclassifiability inside the type object.
    //     But that was just the cleanest design.
    //     (In contrast, lambdas do NOT store information about reclassifiability inside their type
    //     objects, and so several unrelated parts of the compiler have to know how to reclassify them,
    //     and it's not so clean.)
    // 
    // Reclassification of the array literal always is possible; and if any elements didn't support it,
    // then the error is reported on the elements rather than the array literal.
    // Dim x as Mammal() = {new Animal} reports a squiggly on the "new Animal" because it only narrows to Mammal
    // Dim x as Mammal() = {new Car} reports an error squiggly on the "new Car"
    //
    // Dominant type of an array literal is the dominant type of all the elements.
    //
    // {1S,1L} -- Put "Short" and "Long" into the dominant type algorithm, and pick "Long" as the dominant type.
    //            This literal array type can be reclassified to any T() or IEnumerable(Of T) or the like
    //            and that will involve converting each element to that T (perhaps producing errors in the process).
    //            e.g. it can be reclassified to IEnumerable(Of String) and will then produce errors that 1S and 1L
    //            can't be converted to string
    //
    // {1S, 999999L} -- Dominant type algorithm is the same: it picks "Long".
    //                  Again, this literal array type can be reclassified to any T() or IEnumerable(Of T) or
    //                  the like. For instance, even though it has dominant type Long, it can still be
    //                  reclassified to Integer().
    //
    // {CShort(1),1L} -- For dominant type again pick "Long" since it is the dominant type out of Short and Long.
    //
    // {{1S,2S},{3L,4L}} -- This rank-2 array literal has four elements, "1S,2S,3L,4L", with types "Short" and
    //                      "Long", so pick Long as the dominant type. The array literal can be reclassified
    //                      to any T(,). 
    //                      Note: we said that an array literal has no dominant type if its elements cannot be
    //                      converted to Object. In this case its elements are "1S,2S,3L,4L". We do *NOT*
    //                      count "{1S,2S}" or "{3L,4L}" as its elements.
    //
    // {CType({1S,2S},Integer()), CType({3L,4L},Integer())} -- This rank-1 array literal has two elements,
    //                      these being "CType({1S,2S},Integer())" and "CType({3L,4L},Integer())".
    //                      Its dominant type is Integer() since the types of each element are Integer().
    //                      It would be reclassifiable to any T(), and if so would produce errors or not
    //                      depending on how its elements convert to that T. For instance, with Option Strict Off,
    //                      there'll be no errors converting it to Enum()().
    //
    // {CType({1S,2S},Integer()), {3L,4L}} -- This is an erroneously-ranked array literal. We specifically
    //                      do *NOT* consider a potential conversion from the expression {3L,4L} into Integer(),
    //                      even though that conversion would succeed. Instead, we say there are three
    //                      elements in the array literal, these being "CType({1S,2S},Integer())" and "3L" and "4L".
    //                      Is there a dominant type amongst these? -- no. The dominant type would be inferred
    //                      to be Object.
    //
    // {function(x)x} -- Lambdas, AddressOf, Nothing and array literals are special because they have no
    //                   inherent type. In all cases we attempt to infer the type of them in a context
    //                   where the target type is unknown. What that means in practice is that for lambdas
    //                   we infer their types; for array literals we use their already-calculated dominant type;
    //                   and for AddressOf and Nothing we just ignore them.
    //



    ThrowIfNull(pInput);

    bool makeBad = false;
    
    if (HasFlag(flags, ExprMustBeConstant) &&
        // Although only constants are allowed in applied attribute contexts, 1-D arrays are allowed too
        //
        !IsAppliedAttributeContext())
    {
        ReportSemanticError(ERRID_RequiredConstExpr, pInput->TextSpan);
        makeBad = true;
        flags = flags & (~ExprMustBeConstant); // don't report duplicate errors about expression constness..
    }

    // Dev10#580591: Several other flags just don't apply to the contents of an array literal.
    ClearFlag(flags, ExprIsExplicitCallTarget | ExprAccessDefaultProperty | ExprPropagatePropertyReference |
                     ExprSuppressImplicitVariableDeclaration | ExprIsConstructorCall | ExprTypeReferenceOnly);
    // And some do:
    SetFlag(flags, ExprForceRValue);

    ArrayList<unsigned> lengthList;
    
    if (!ValidateShape(pInput, lengthList))
    {
        return AllocateBadExpression(pInput->TextSpan);
    }
    
    ExpressionListHelper elementList(this);
    bool first = false;
    
    if (pInput->Elements)
    {
        ParseTree::InitializerList * pList = pInput->Elements->InitialValues;
        while (pList)
        {
            if (pList->Element)
            {                    
                ILTree::Expression * pExpr = InterpretArrayInitializerElement(pList->Element, pList->TextSpan, flags);

                if (pExpr)
                {
                    if (pExpr->bilop == SX_UNBOUND_LAMBDA)
                    {
                        //Normally, the check to see if a lambda parameter shadows a local variable
                        //is done inside InterpretUnboundLambda, while we are converting an unbound 
                        //lambda to a bound lambda. In the case of array literals, however, we want to do 
                        //that check upfront, and mark a lambda array literal element as bad if that condition 
                        //occurs. We do this becuase the shadowed lambda parameter is a problem intrinsic to the lambda,
                        //rather than a problem that may result from attempting to convert the lambda to a particular delegate type.
                        //We want to catch intrinsic problems with the lambda and report them as such, rather than giving a generic 
                        //"cannot convert this thingy" error on the array literal as a whole.
                        CheckLambdaParameterShadowing(&pExpr->AsUnboundLambdaExpression());
                    }
                    elementList.Add(pExpr, pExpr->Loc);
                }

                if (IsBad(pExpr))
                {
                    MakeBad(elementList.Start());
                }
            }

            pList = pList->Next;
        }
    }
    
    
    if (HasFlag(flags, ExprCreateNestedArrayLiteral) )
    {
        // Nested literals are simple: they don't need to worry about type inference or any of that...
        ILTree::NestedArrayLiteralExpression *pRet = AllocateNestedArrayLiteralExpression(elementList.Start(), pInput->TextSpan);
        // All nested array literals are marked just with the type "void".
        return makeBad ? MakeBad(pRet) : pRet;
    }


    // But for non-nested array literals, we will have to worry about type inference and conversion:
    ILTree::ArrayLiteralExpression *pRet = AllocateArrayLiteralExpression(
                                                elementList.Start(),
                                                lengthList,
                                                pInput->TextSpan);

    // Here's the type inference. It will update "pRet->NumDominantCandidates" according to whether it had to
    // pick Object as the dominant type for lack of a better alternative.
    ILTree::Expression *dominantTypeWinner;
    BCSYM *dominantType = InferDominantTypeOfExpressions(
                                pRet->NumDominantCandidates,
                                dominantTypeWinner,  // not used
                                (ConstIterator<ILTree::Expression*>)ArrayElementIterator(pRet));

    pRet->ResultType = m_SymbolCreator.GetArrayLiteralType(
                                pRet->Rank,
                                dominantType,
                                (ConstIterator<ILTree::Expression*>)ArrayElementIterator(pRet),
                                pRet->Loc);

    // What will ultimately happen is that this array literal will be converted to a real array, along with all its elements.
    // and any errors or warnings in that process will be reported. This is an operation which may mutate
    // the SX_tree (e.g. by turning function(x)x into function(x as Object)x), and will turn SX_ADDRESSOF
    // into properly resolved things, and may report warnings/errors on the conversions of each element.
    // Moreover, if we're merely asked to convert our array into its own inferred dominant type, then
    // we may have to produce warnings ("object assumed for array") or errors ("no dominant type found"),
    // and MakeBad in case of the latter.
    // But all of that work is done later, by other people. We can't do it here because we don't know whether
    // our caller has a target type in mind or not.

    VSASSERT(dominantType!=NULL && pRet->ResultType!=NULL, "unexpected: InferDominantTypeOfExpressions and GetArrayLiteralType should both return non-NULLs");

    return makeBad ? MakeBad(pRet) : pRet;
}


ILTree::ArrayLiteralExpression* 
Semantics::AllocateArrayLiteralExpression
(
    ILTree::ExpressionWithChildren * pElementList,
    const ArrayList<unsigned> & lengthList, 
    const Location & loc
)
{
    ILTree::ArrayLiteralExpression * pRet =
        (ILTree::ArrayLiteralExpression *)
        AllocateExpression
        (
            SX_ARRAYLITERAL, 
            m_SymbolCreator.GetVoidType(), 
            loc
        );

    pRet->ElementList = pElementList;
    pRet->Rank = lengthList.Count();

    if (pElementList)
    {
        pRet->uFlags |= SF_INHERIT(pElementList->uFlags);
    }

    unsigned * pDims = new (*m_SymbolCreator.GetNorlsAllocator()) unsigned[pRet->Rank];

    for (unsigned i = 0; i < pRet->Rank; ++i)
    {
        pDims[i] = lengthList[i];
    }

    pRet->Dims = pDims;
    pRet->ResultType = m_SymbolCreator.GetVoidType();
    pRet->vtype = t_array;
    
    return pRet;
}

ILTree::ArrayLiteralExpression* 
Semantics::AllocateArrayLiteralExpression
(
    ILTree::ExpressionWithChildren * pElementList,
    unsigned rank,
    unsigned * pDims,
    const Location & loc
)
{
    ILTree::ArrayLiteralExpression * pRet =
        (ILTree::ArrayLiteralExpression *)
        AllocateExpression
        (
            SX_ARRAYLITERAL, 
            m_SymbolCreator.GetVoidType(), 
            loc
        );

    pRet->ElementList = pElementList;
    pRet->Rank = rank;
    pRet->Dims = pDims;
    pRet->ResultType = m_SymbolCreator.GetVoidType();
    pRet->vtype = t_array;

    if (pElementList)
    {
        pRet->uFlags |= SF_INHERIT(pElementList->uFlags);
    }

    return pRet;
    
}

ILTree::NestedArrayLiteralExpression *
Semantics::AllocateNestedArrayLiteralExpression
(
    ILTree::ExpressionWithChildren * pElementList,
    const Location & loc
)
{
    ILTree::NestedArrayLiteralExpression * pRet =
        (ILTree::NestedArrayLiteralExpression *)
        AllocateExpression
        (
            SX_NESTEDARRAYLITERAL,
            m_SymbolCreator.GetVoidType(),
            loc
        );

    pRet->ElementList = pElementList;

    if (pElementList)
    {
        pRet->uFlags |= SF_INHERIT(pElementList->uFlags);
    }


    return pRet;
}


ILTree::Expression *
Semantics::InterpretArrayInitializerElement
(
    ParseTree::Initializer * pInitializer,
    const Location & loc, //NOTE: this location is only valid for reporting "internal compiler error" messages only.
    ExpressionFlags flags
)
{
    ThrowIfNull(pInitializer);

    switch (pInitializer->Opcode)
    {
        case ParseTree::Initializer::Expression:
            if (pInitializer->AsExpression()->Value->Opcode == ParseTree::Expression::ArrayInitializer)
            {
                flags |= ExprCreateNestedArrayLiteral;
            }
            else
            {
                flags &= ~ExprCreateNestedArrayLiteral;
            }

            if (pInitializer->AsExpression()->Value->Opcode == ParseTree::Expression::Lambda)
            {
                flags|= ExprDontInferResultType;
            }

            return InterpretExpression(pInitializer->AsExpression()->Value, flags);
        case ParseTree::Initializer::Deferred:
            if (pInitializer->AsDeferred()->Value)
            {
                return InterpretArrayInitializerElement(pInitializer->AsDeferred()->Value, loc, flags);
            }
            else
            {
                VSFAIL(L"Deferred initializer has no expression!");
                ReportSemanticError(ERRID_InternalCompilerError, loc);
                return AllocateBadExpression(m_SymbolCreator.GetVoidType(), loc);
            }
        default:
            //this includes Assignment initializers....
            VSFAIL(L"Unexpected initializer type!");
            ReportSemanticError(ERRID_InternalCompilerError,loc);
            return AllocateBadExpression(m_SymbolCreator.GetVoidType(), loc);
    }
}







BCSYM *
Semantics::InferDominantTypeOfExpressions
(
    _Out_ unsigned &NumCandidates,
    _Out_ ILTree::Expression* &winner, 
    _In_ ConstIterator<ILTree::Expression *> &expressions
)
{
    // Arguments: "expressions" is a list of expressions from which to infer dominant type
    // Output: we might return Void / NumCandidates==0 (if we couldn't find a dominant type)
    // Or we might return Object / NumCandidates==0 (if we had to assume Object because no dominant type was found)
    // Or we might return Object / NumCandidates>=2 (if we had to assume Object because multiple candidates were found)
    // Or we might return a real dominant type from one of the candidates / NumCandidates==1
    // In the last case, "winner" is set to one of the expressions who proposed that winning dominant type.
    // "Winner" information might be useful if you are calculating the dominant type of "{}" and "{Nothing}"
    // and you need to know who the winner is so you can report appropriate warnings on him.


    // The dominant type of a list of elements means:
    // (1) for each element, attempt to classify it as a value in a context where the target
    // type is unknown. So unbound lambdas get classified as anonymous delegates, and array literals get
    // classified according to their dominant type (if they have one), and Nothing is ignored, and AddressOf too.
    // But skip over empty array literals.
    // (2) Consider the types of all elements for which we got a type, and feed these into the dominant-type
    // algorithm: if there are multiple candidates such that all other types convert to it through identity/widening,
    // then pick the dominant type out of this set. Otherwise, if there is a single all-widening/identity candidate,
    // pick this. Otherwise, if there is a single all-widening/identity/narrowing candidate, then pick this.
    // (3) Otherwise, if the dominant type algorithm has failed and every element was an empty array literal {}
    // then pick Object() and report a warning "Object assumed"
    // (4) Otherwise, if every element converts to Object, then pick Object and report a warning "Object assumed".
    // (5) Otherwise, there is no dominant type; return Void and report an error.

    DBG_SWITCH_PRINTF(fDumpInference, L"Dominant type of a list of expressions:\n");

    NumCandidates = 0;
    winner = NULL;
    unsigned long Count=0, CountOfEmptyArrays=0;  // To detect case (3)
    ILTree::Expression *AnEmptyArray = NULL;  // Used for case (3), so we'll return one of them
    bool AllConvertibleToObject = true;  // To detect case (4)

    TypeInferenceCollection typeList(this, &m_TreeStorage, &NorlsAllocWrapper(&m_TreeStorage));

    while (expressions.MoveNext())
    {
        Count++;
        ILTree::Expression *expression = expressions.Current();
        ThrowIfNull(expression);
        ThrowIfNull(expression->ResultType);
        VSASSERT(expression->bilop != SX_NESTEDARRAYLITERAL, "unexpected: an SX_NESTEDARRAYLITERAL in a list of expressions");
        #if DEBUG
        StringBuffer buffer;
        #endif

        
        BCSYM *expressionType = expression->ResultType;

        if (expression->bilop == SX_UNBOUND_LAMBDA)
        {
            // Dev10#464270: for any lambdas "function(x)x+1" which lack a ByVal clause on their
            // parameters, type inference will change then to "function(x As Object)x+1". We don't want that,
            // not when we're just inferring dominant type! So instead we'll do our inference on a copy of the lambda.
            // on a copy of the list:
            ILTree::Expression *expression2 = m_TreeAllocator.xShallowCopyUnboundLambdaTreeForScratch(&expression->AsUnboundLambdaExpression());
            expressionType = InferLambdaType(&expression2->AsUnboundLambdaExpression(), expression2->Loc);
        }
        else if (expression->bilop == SX_ARRAYLITERAL && expressionType->IsArrayLiteralType())
        {
            expressionType = expressionType->DigThroughArrayLiteralType(&m_SymbolCreator);
            // Here we're converting it to an array type. If it was a typeless array literal,
            // e.g. {AddressOf Main}, then it'll become an array of void. This case is caught
            // in the following block of code, and doesn't add anything to the dominant type inference.
        }

        if (expression->bilop==SX_NOTHING)
        {
            // Note: SX_NOTHINGs should notionally have Void type. But actually they're usually given Object type.
            // That's why we treat them specially.
            DBG_SWITCH_PRINTF(fDumpInference, L"  element <%S> : no type\n", ILTree::BilopName(expression->bilop));
        }
        else if (expression->bilop==SX_ARRAYLITERAL && IsEmptyArrayLiteralType(expression->ResultType))
        {
            // Empty array literals {} should not be constraints on the dominant type algorithm.
            DBG_SWITCH_PRINTF(fDumpInference, L"  skipping {} <%S> : type %s\n", ILTree::BilopName(expression->bilop), ExtractErrorName(expressionType, buffer));
            CountOfEmptyArrays++;
            AnEmptyArray = expression;
        }
        else if (expressionType!=NULL && !expressionType->IsVoidType() &&
                 !(expressionType->IsArrayType() && expressionType->PArrayType()->GetRoot()->IsVoidType()))
        {
            DBG_SWITCH_PRINTF(fDumpInference, L"  populating with <%S> : %s\n", ILTree::BilopName(expression->bilop), ExtractErrorName(expressionType, buffer));
            typeList.AddType(expressionType, ConversionRequired::Any, expression);

            if (IsRestrictedType(expression->ResultType, m_CompilerHost))
            {
                DBG_SWITCH_PRINTF(fDumpInference, L"  this element is a restricted type; not convertible to object\n");
                AllConvertibleToObject = false;
            }
        }
        else
        {
            DBG_SWITCH_PRINTF(fDumpInference, L"  element <%S> : no type; not convertible to object\n", ILTree::BilopName(expression->bilop));
            AllConvertibleToObject = false;
            // this will pick up lambdas which couldn't be inferred, and array literals which lacked a dominant type,
            // and AddressOf expressions.
        }

    }


    // Here we calculate the dominant type.
    // Note: if there were no candidate types in the list, this will fail with errorReason = NoneBest.
    InferenceErrorReasons errorReasons = InferenceErrorReasonsOther;
    DominantTypeDataList results(&m_TreeStorage);
    typeList.FindDominantType(results, &errorReasons, true);  // "true" = IgnoreOptionStrict
    #if DEBUG
    StringBuffer buffer;
    #endif

    if (results.Count() == 1 && errorReasons == InferenceErrorReasonsOther)
    {
        // main case: we succeeded in finding a domiannt type
        DBG_SWITCH_PRINTF(fDumpInference, L"  Inferred dominant type %s.\n", ExtractErrorName(results[0]->ResultType, buffer));
        VSASSERT(results[0]->SourceExpressions.Count()>0, "unexpected: the winning candidate claims not to have come from any expressions, even though the only candidates we put in came from expressions");
        // There might have been several joint winners. We're only asked to provide one "witness", for
        // warning-reporting purposes. The idea is that if all winners were "object assumed" then we provide
        // one of them so that its warning can be displayed. But if at least one winner was not "object assumed"
        // then we provide that. That way the caller will know there's no need to display a warning.
        winner = results[0]->SourceExpressions[0];
        for (unsigned long i=0; i<results[0]->SourceExpressions.Count(); i++)
        {
            ILTree::Expression *otherWinner = results[0]->SourceExpressions[i];
            if (otherWinner->bilop!=SX_ARRAYLITERAL || otherWinner->AsArrayLiteralExpression().NumDominantCandidates==1)
            {
                winner = otherWinner;
            }
        }
        VSASSERT(!results[0]->ResultType->IsVoidType(), "internal logic error: how could void have won the dominant type algorithm?");
        NumCandidates = 1;
        return results[0]->ResultType;
    }
    else if (Count == CountOfEmptyArrays && Count>0)
    {
        // special case: the dominant type of a list of empty arrays is Object(), not Object.
        DBG_SWITCH_PRINTF(fDumpInference, L"  Warning: assumed Object() dominant type because everything was an empty array.\n");
        VSASSERT(AnEmptyArray!=NULL, "internal logic error: if we got at least one empty array, then AnEmptyArray should not be null");
        winner = AnEmptyArray;
        NumCandidates = 1;
        return m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetObjectType());
    }
    else if (AllConvertibleToObject && (errorReasons & InferenceErrorReasonsAmbiguous))
    {
        // special case: there were multiple dominant types, so we fall back to Object
        DBG_SWITCH_PRINTF(fDumpInference, L"  Warning: assumed Object dominant type because there were multiple candidates.\n");
        VSASSERT(results.Count()>1, "internal logic error: if InferenceErrorReasonsAmbiguous, you'd have expected more than 1 candidate");
        NumCandidates = results.Count();
        return GetFXSymbolProvider()->GetObjectType();
    }
    else if (AllConvertibleToObject)
    {
        // fallback case: we didn't find a dominant type, but can fall back to Object
        DBG_SWITCH_PRINTF(fDumpInference, L"  Warning: assumed Object dominant type.\n");
        NumCandidates = 0;
        return GetFXSymbolProvider()->GetObjectType();
    }
    else
    {
        DBG_SWITCH_PRINTF(fDumpInference, L"  Error: no dominant type. Returning 'Void'.\n");
        NumCandidates = 0;
        return m_SymbolCreator.GetVoidType();
    }

}



BCSYM *
Semantics::InferDominantTypeOfExpressions
(
    _Out_ unsigned &NumCandidates,
    _Out_ ILTree::Expression* &winner,
    _In_opt_ ILTree::Expression *expression1,
    _In_opt_ ILTree::Expression *expression2,
    _In_opt_ ILTree::Expression *expression3
)
{
    ArrayList<ILTree::Expression*> expressions;
    if (expression1!=NULL)
    {
        expressions.Add(expression1);
    }
    if (expression2!=NULL)
    {
        expressions.Add(expression2);
    }
    if (expression3!=NULL)
    {
        expressions.Add(expression3);
    }
    ConstIterator<ILTree::Expression*> iterator(expressions.GetIterator());
    return InferDominantTypeOfExpressions(NumCandidates, winner, iterator);
}


ILTree::Expression *
Semantics::ConvertExpressionToDominantType
(
    _In_ ILTree::Expression *Expression,
    _In_ BCSYM *ResultType,
    _In_ ILTree::Expression *DominantWinnerExpression,
    _In_ ExpressionFlags Flags
)
{
    if (Expression == DominantWinnerExpression)
    {
        return ConvertWithErrorChecking(Expression, NULL, Flags | ExprForceRValue);
    }
    else
    {
        return ConvertWithErrorChecking(Expression, ResultType, Flags | ExprForceRValue);
    }
}


//+--------------------------------------------------------------------------
//  Method:     Semantics.InterpretGetType
//
//  Description:    Interprets the GetType expression
//
//  Parameters:
//      [UnboundType]   --  The unbound type to create a Type object of
//      [Flags]     --  Expresion eval flags
//
//  History:    3-2-2005    Microsoft     Moved code from InterpretExpression
//---------------------------------------------------------------------------
ILTree::Expression *
Semantics::InterpretGetType
(
    ParseTree::GetTypeExpression * UnboundType,
    ExpressionFlags Flags
)
{
    bool DisallowOpenTypes = false;

    if (HasFlag(Flags, ExprMustBeConstant))
    {
        if (IsAppliedAttributeContext())
        {
            // When constant types are requirements, open types i.e. types
            // involving type parameters are not allowed.
            //
            DisallowOpenTypes = true;
        }
        else
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                UnboundType->TextSpan);

            return AllocateBadExpression(UnboundType->TextSpan);
        }
    }

    // Get the type whose meta type is needed.
    bool TypeIsBad = false;
    Type *SourceType =
        InterpretTypeName(UnboundType->AsGetType()->TypeToGet, TypeIsBad, TypeResolveAllowAllTypes);

    if (TypeIsBad)
    {
        return AllocateBadExpression(UnboundType->TextSpan);
    }

    return InterpretGetType(SourceType, DisallowOpenTypes, UnboundType->TextSpan, UnboundType->TypeToGet->TextSpan);
}


//+--------------------------------------------------------------------------
//  Method:     Semantics.InterpretGetType
//
//  Description:    Interprets the GetType expression, based on bound type
//
//  Parameters:
//      [SourceType]    --  The bound type to create a Type object of
//      [DisallowOpenTypes]     --  Should generic parameters be forbidden
//      [ExpressionLocation]    --  The loc of the entire GetType(type) expression
//      [TypeLocation]  --  The loc of the value passed to GetType
//
//  History:    3-2-2005    Microsoft     Moved code from InterpretExpression
//---------------------------------------------------------------------------
ILTree::Expression *
Semantics::InterpretGetType
(
    Type * SourceType,
    bool DisallowOpenTypes,
    const Location &ExpressionLocation,
    const Location &TypeLocation
)
{
    ILTree::Expression * Result = NULL;

    if (TypeHelpers::IsArrayType(SourceType) &&
        SourceType->ChaseToType() == GetFXSymbolProvider()->GetType(FX::VoidType))
    {
        ReportSemanticError(
            ERRID_VoidArrayDisallowed,
            TypeLocation);

        return AllocateBadExpression(ExpressionLocation);
    }

    if (DisallowOpenTypes &&
        RefersToGenericParameter(SourceType))
    {
        // "Types constructed with type parameters are disallowed in attribute arguments."

        ReportSemanticError(
            ERRID_OpenTypeDisallowed,
            TypeLocation);

        return AllocateBadExpression(ExpressionLocation);
    }

    if (!GetFXSymbolProvider()->IsTypeAvailable(FX::TypeType))
    {
        ReportMissingType(FX::TypeType, ExpressionLocation);
        return AllocateBadExpression(ExpressionLocation);
    }

    Result =
        AllocateExpression(
            SX_METATYPE,
            GetFXSymbolProvider()->GetTypeType(),
            AllocateExpression(
                SX_NOTHING,
                SourceType,
                TypeLocation),
            ExpressionLocation);

    return Result;
}


//+--------------------------------------------------------------------------
//  Method:     Semantics.InterpretGetXmlNamespace
//
//  Description:    Interprets the GetXmlNamespace expression
//
//  Parameters:
//      [UnboundPrefix] --  The unbound prefix to create an XmlNamespace from
//      [Flags]         --  Expresion eval flags
//
//  History:    7-26-2006    Microsoft     Created
//---------------------------------------------------------------------------
ILTree::Expression *
Semantics::InterpretGetXmlNamespace
(
    ParseTree::GetXmlNamespaceExpression * UnboundPrefix,
    ExpressionFlags Flags
)
{
    if (HasFlag(Flags, ExprMustBeConstant))
    {
        ReportSemanticError(
            ERRID_RequiredConstExpr,
            UnboundPrefix->TextSpan);

        return AllocateBadExpression(UnboundPrefix->TextSpan);
    }

    // Validate the prefix
    if (UnboundPrefix->Prefix.IsBad)
    {
        return AllocateBadExpression(UnboundPrefix->TextSpan);
    }

    if (StringPool::StringLength(UnboundPrefix->Prefix.Name) != 0 &&
        !ValidateXmlName(UnboundPrefix->Prefix.Name, UnboundPrefix->Prefix.TextSpan, m_Errors))
    {
        return AllocateBadExpression(UnboundPrefix->TextSpan);
    }

    Declaration * XmlPrefix = InterpretXmlPrefix(NULL, UnboundPrefix->Prefix.Name, NameSearchDonotResolveImportsAlias, m_SourceFile);

    if (!XmlPrefix)
    {
        ReportSemanticError(
            ERRID_UndefinedXmlPrefix,
            UnboundPrefix->Prefix.TextSpan,
            UnboundPrefix->Prefix.Name);

        return AllocateBadExpression(UnboundPrefix->TextSpan);
    }

    ParserHelper PH(&m_TreeStorage, UnboundPrefix->TextSpan);
    ParseTree::Expression *PrefixExpr;

#if IDE
    if (m_IsGeneratingXML && StringPool::StringLength(UnboundPrefix->Prefix.Name) != 0)
    {
        // Create symbol reference for the Intellisense case (so that Intellisense
        // code path can directly access the symbol rather than looking it up)
        PrefixExpr = PH.CreateBoundExpression(
            AllocateSymbolReference(XmlPrefix, GetFXSymbolProvider()->GetStringType(), NULL, UnboundPrefix->TextSpan));
    }
    else
#endif
    {
        // Use literal string representation of the namespace
        PrefixExpr = PH.CreateStringConst(XmlPrefix->PAlias()->GetSymbol()->PXmlNamespaceDeclaration()->GetName());
    }

    return
        InterpretExpression(
            PH.CreateMethodCall(
                PH.CreateQualifiedNameExpression(
                    PH.CreateGlobalNameSpaceExpression(),
                    5,
                    STRING_CONST(m_Compiler, ComDomain),
                    STRING_CONST(m_Compiler, ComXmlDomain),
                    STRING_CONST(m_Compiler, ComLinqDomain),
                    STRING_CONST(m_Compiler, ComXmlNamespace),
                    STRING_CONST(m_Compiler, XmlGetMethod)),
                PH.CreateArgList(1, PrefixExpr)),
            Flags);
}

ILTree::Expression *
Semantics::ApplyContextSpecificSemantics
(
    ILTree::Expression *Result,
    ExpressionFlags Flags,
    Type *TargetType
)
{
    if (IsBad(Result))
    {
        return Result;
    }

    if (HasFlag(Flags, ExprAccessDefaultProperty))
    {
        // If Result is a property reference, that property might produce a value with a
        // default property. It's only appropriate to interpret the property reference in
        // contexts that expect an RValue. Cases that need to access default properties of
        // properties deal with this explicitly.

        if (IsPropertyReference(Result) && HasFlag(Flags, ExprForceRValue))
        {
            Result = FetchFromProperty(Result);

            if (IsBad(Result))
            {
                return Result;
            }
        }

        Result = AccessDefaultProperty(
            Result->Loc,
            Result,
            chType_NONE,
            Flags);

        if (IsBad(Result))
        {
            return Result;
        }
    }

    if (HasFlag(Flags, ExprForceRValue))
    {
        Result = MakeRValue(Result, TargetType);

        if (IsBad(Result))
        {
            return Result;
        }
    }

    if (HasFlag(Flags, ExprIsAssignmentTarget) &&
        !HasFlag32(Result, SXF_LVALUE) &&
        !(IsPropertyReference(Result) &&
          // Rule out references to properties of non-lvalue value types.
          !(Result->AsPropertyReferenceExpression().Left->bilop == SX_SYM &&
            Result->AsPropertyReferenceExpression().Left->AsSymbolReferenceExpression().BaseReference &&
            !HasFlag32(Result->AsPropertyReferenceExpression().Left->AsSymbolReferenceExpression().BaseReference, SXF_LVALUE) &&
            TypeHelpers::IsValueType(Result->AsPropertyReferenceExpression().Left->AsSymbolReferenceExpression().BaseReference->ResultType)&&
            // make sure Me.SomeProperty() still works for value types(
            !(Result->AsPropertyReferenceExpression().Left->AsSymbolReferenceExpression().BaseReference->bilop == SX_SYM &&
             Result->AsPropertyReferenceExpression().Left->AsSymbolReferenceExpression().BaseReference->AsSymbolReferenceExpression().Symbol->PVariable()->IsMe()))))
    {
        ReportAssignmentToRValue(Result);
        return MakeBad(Result);
    }

    if (IsPropertyReference(Result) && !HasFlag(Flags, ExprPropagatePropertyReference))
    {
        Result = FetchFromProperty(Result);

        if (IsBad(Result))
        {
            return Result;
        }
    }

    return Result;
}

ILTree::Expression *
Semantics::InterpretExpressionWithTargetType
(
    ParseTree::Expression *Input,
    ExpressionFlags Flags,
    Type *TargetType,
    Type **OriginalType
)
{
    ExpressionFlags ExprFlags = Flags;
    if (TargetType != NULL)
    {
        ExprFlags |= ExprDontInferResultType;

        // Set this flag, if we ned up interpretting a 
        // lambda, we want to get its return type from 
        // the target type.
        Flags |= ExprGetLambdaReturnTypeFromDelegate;
    }
    if (HasFlag(Flags, ExprInferResultTypeExplicit))
    {
        // remove not infer flag if we explicity want to infer regardless of resulttype.
        ExprFlags &= ~ExprDontInferResultType;
    }

    ILTree::Expression * Result = InterpretExpression(Input, ExprFlags, 0, 0, TargetType);

    if (OriginalType)
    {
        // Dev10#738311: Since we didn't pass through TargetType above, we also need to special-case the "Nothing" literal here:
        // (that's because the Nothing literal has no original type of its own: its original type depends on
        // the target type of the surrounding context)
        if (Result->ResultType != NULL && !TypeHelpers::IsVoidType(Result->ResultType) &&
            Input->Opcode != ParseTree::Expression::Nothing)
        {
            *OriginalType = Result->ResultType;
        }
        else
        {
            *OriginalType = TargetType;
        }
    }

    if (TargetType && !IsBad(Result))
    {    
        Result = ConvertWithErrorChecking(Result, TargetType, Flags);
    }

    return Result;
}

#if IDE  
Identifier *
Semantics::SynthesizeQualifiedName
(
    ParseTree::QualifiedExpression *Input,
    bool &GlobalQualified
)
{
    VSASSERT(Input->Name->Opcode == ParseTree::Expression::Name,"Caller must check that qualified name is literal");

    if (Input->Base->Opcode == ParseTree::Expression::GlobalNameSpace)
    {
        GlobalQualified = true;
        return Input->Name->AsName()->Name.Name;
    }

    return
        m_Compiler->ConcatStrings(
            Input->Base->Opcode == ParseTree::Expression::DotQualified ?
                SynthesizeQualifiedName(Input->Base->AsQualified(), GlobalQualified) :
                Input->Base->AsName()->Name.Name,
            L".",
            Input->Name->AsName()->Name.Name);
}
#endif

static bool
IsFieldOfMarshalByRefObject
(
    ILTree::Expression *Value
)
{
    return
        Value->bilop == SX_SYM &&
        Value->AsSymbolReferenceExpression().BaseReference &&
        Value->AsSymbolReferenceExpression().BaseReference->ResultType->IsClass() &&
        Value->AsSymbolReferenceExpression().BaseReference->ResultType->PClass()->DerivesFromMarshalByRef() &&
        // A reference through Me doesn't count.
        !IsMeReference(Value->AsSymbolReferenceExpression().BaseReference);
}

ILTree::Expression *
Semantics::InterpretQualifiedExpression
(
    ILTree::Expression *BaseReference,
    _In_z_ Identifier *Name,
    ParseTree::Expression::Opcodes Opcode,
    const Location &TextSpan,
    ExpressionFlags Flags,
    int GenericTypeArity
)
{
    // Create a constant Name expression
    ParseTree::NameExpression NameExpr;
    NameExpr.Opcode = ParseTree::Expression::Name;
    NameExpr.TextSpan = TextSpan;
    NameExpr.Name.Name = Name;
    NameExpr.Name.TypeCharacter = chType_NONE;
    NameExpr.Name.IsBracketed = false;
    NameExpr.Name.IsBad = false;
    NameExpr.Name.IsNullable = false;
    NameExpr.Name.TextSpan = TextSpan;

    return
        InterpretQualifiedExpression(
            BaseReference,
            &NameExpr,
            Opcode,
            TextSpan,
            Flags,
            GenericTypeArity);
}

ILTree::Expression *
Semantics::SetupLookupEnviornmentForQualifiedExpressionInterpretation
(
    ILTree::Expression * & BaseReference,
    Scope * & MemberLookup,
    bool & BaseReferenceIsNamespace,
    Type * & BaseReferenceType,
    GenericParameter * & TypeParamToLookupInForMember,
    Type * & TypeForGenericBinding,
    ParseTree::Expression::Opcodes Opcode,
    const Location & TextSpan
)
{
    // If the base reference names a class, module, enum,
    // or namespace then it specifies a lookup scope but
    // does not provide an object reference.

    if (BaseReference->bilop == SX_SYM)
    {
        Declaration *BaseReferenceSymbol = BaseReference->AsSymbolReferenceExpression().Symbol;

        if (BaseReferenceSymbol->IsType())
        {
            if (TypeHelpers::IsGenericParameter(BaseReferenceSymbol))
            {
                // Type parameters cannot be used as qualifiers.

                ReportSemanticError(
                    ERRID_TypeParamQualifierDisallowed,
                    BaseReference->Loc);

                return AllocateBadExpression(TextSpan);
            }

            VSASSERT(Opcode == ParseTree::Expression::DotQualified, "Type reference unexpected as base expression for anything but dot qualified expression!!!");
            //
            // Warn for types via instance, i.e expression.Type.sharedMember(VSW 211774)
            // However the expression could be My generated like Form1.NestedType.shared. 'Form1' shows here as
            // a call to the default inst property that returns a Form1 inst. The warning would be wrong in this case
            // VSW487655
            ILTree::Expression *bbRef = BaseReference->AsSymbolReferenceExpression().BaseReference;
            if ( bbRef &&
                !bbRef->NameCanBeType &&
                !(bbRef->bilop == SX_CALL &&
                  bbRef->AsCallExpression().Left->bilop == SX_SYM &&
                  bbRef->AsCallExpression().Left->AsSymbolReferenceExpression().pnamed->IsProc() &&
                  bbRef->AsCallExpression().Left->AsSymbolReferenceExpression().pnamed->PProc()->IsMyGenerated()))
            {
                ReportSemanticError(
                    WRNID_SharedMemberThroughInstance,
                    BaseReference->Loc);
            }

            BaseReference = NULL;
        }

        else if (IsNamespace(BaseReferenceSymbol))
        {
            MemberLookup = ViewAsScope(BaseReferenceSymbol->PNamespace());
            BaseReferenceIsNamespace = true;

            VSASSERT(Opcode == ParseTree::Expression::DotQualified, "Namespace reference unexpected as base expression for anything but dot qualified expression!!!");
            BaseReference = NULL;
        }
    }

    // In a dot-qualified context, intrinsic types act exactly as
    // their object equivalents. This allows calling methods on String,
    // Object, Integer, etc.

    if (BaseReference &&
        // A MyBase expression shouldn't chase through to the primary interface here.
        (BaseReference->bilop != SX_SYM ||
         !HasFlag32(BaseReference, SXF_SYM_MYBASE | SXF_SYM_MYCLASS)))
    {
        if (TypeHelpers::IsArrayType(BaseReferenceType))
        {
            BaseReferenceType = GetFXSymbolProvider()->GetType(FX::ArrayType);
        }

        if (TypeHelpers::IsVoidType(BaseReferenceType))
        {
            ReportSemanticError(
                ERRID_VoidValue,
                BaseReference->Loc);

            return AllocateBadExpression(TextSpan);
        }
    }

    if (TypeHelpers::IsGenericParameter(BaseReferenceType))
    {
        TypeForGenericBinding = BaseReferenceType;
        MemberLookup = NULL;
        TypeParamToLookupInForMember = BaseReferenceType->PGenericParam();
    }
    else if (TypeHelpers::IsClassOrInterfaceType(BaseReferenceType) ||
             TypeHelpers::IsValueType(BaseReferenceType))
    {
        VSASSERT(BaseReferenceType->IsContainer(), "Non-container base reference type unexpected!!!");

        TypeForGenericBinding = BaseReferenceType;
        MemberLookup = ViewAsScope(BaseReferenceType->PContainer());
    }

    return NULL;
}

ILTree::Expression *
Semantics::InterpretQualifiedExpression
(
    ILTree::Expression *BaseReference,
    ParseTree::Expression *Name,
    ParseTree::Expression::Opcodes Opcode,
    const Location &TextSpan,
    ExpressionFlags Flags,
    int GenericTypeArity
)
{
    ILTree::Expression *Result = NULL;

    ILTree::Expression *OriginalBaseReference = BaseReference;
    Type *BaseReferenceType = BaseReference->ResultType;
    bool BaseReferenceIsNamespace = false;
    Scope *MemberLookup = NULL;
    GenericParameter *TypeParamToLookupInForMember = NULL;
    bool MemberIsBad = false;
    Type *TypeForGenericBinding = NULL;
    bool LookingForAQueryOperator = HasFlag(Flags, ExprIsQueryOperator);

    ClearFlag(Flags, ExprIsQueryOperator);

    RETURN_IF_NOT_NULL
    (
        ILTree::Expression *,
        SetupLookupEnviornmentForQualifiedExpressionInterpretation
        (
            BaseReference,
            MemberLookup,
            BaseReferenceIsNamespace,
            BaseReferenceType,
            TypeParamToLookupInForMember,
            TypeForGenericBinding,
            Opcode,
            TextSpan
        )
    );

    GenericBinding *GenericBindingContext = NULL;
    Symbol *Member = NULL;
    ParseTree::IdentifierDescriptor *MemberIdentifier = NULL;


    // If the name is constant, then attempt to perform early bound lookup
    if (Name->Opcode == ParseTree::Expression::Name)
    {
        MemberIdentifier = &Name->AsName()->Name;
        VSASSERT(!MemberIdentifier->IsBad, "The member identifier should never be bad, as a SyntaxError should be created in this case instead");
    }
    else if (Name->Opcode == ParseTree::Expression::SyntaxError)
    {
        MemberIsBad = true;
    }

    if ((MemberLookup || TypeForGenericBinding) &&
        MemberIdentifier && Opcode == ParseTree::Expression::DotQualified)
    {
        VSASSERT(!MemberIsBad, "If MemberIdentifier is specified, it must not be bad (should be SyntaxError in that case)");

        Type *AccessingInstanceType = InstanceTypeOfReference(OriginalBaseReference);

        if (StringPool::IsEqual(MemberIdentifier->Name, STRING_CONST(m_Compiler, New)) &&
            !MemberIdentifier->IsBracketed &&
            TypeHelpers::IsClassOrRecordType(BaseReferenceType) &&
            !TypeHelpers::IsEnumType(BaseReferenceType))
        {
            Member = BaseReferenceType->PClass()->GetFirstInstanceConstructor(m_Compiler);

            if (Member == NULL)
            {
                ReportSemanticError(
                    ERRID_ConstructorNotFound1,
                    TextSpan,
                    BaseReferenceType);

                return AllocateBadExpression(TextSpan);
            }

            ThrowIfFalse(Member->IsNamedRoot());

            CreateNameLookupGenericBindingContext(Member->PNamedRoot(), BaseReferenceType->PClass(), &GenericBindingContext);

            UpdateNameLookupGenericBindingContext(TypeForGenericBinding, &GenericBindingContext);

            CheckAccessibility(Member->PNamedRoot(), GenericBindingContext, TextSpan, NameNoFlags, AccessingInstanceType, MemberIsBad);
        }
        else
        {
            bool ignored = false;
            NameFlags NameLookupFlags = 0;
            SetFlag(NameLookupFlags, NameSearchIgnoreParent);

            // This is a terrible hack. Because anonymous types use synthetic code gen
            // to emit its methods, and because fields are named $Field, the anonymous types
            // code uses _Field in the codegen, and in InterpretName, we hack _Field to be
            // $Field. But we don't want to do this for all name interpretations, because
            // arguments to the anonymous type constructor are the field names. Since here,
            // we are binding a field, we'll mark it so. And thus cause InterpretName to
            // do the hack. This is terrible. But without re-writing the anonymous type code
            // to build methods without synthetic code gen, this is what we must do.

            if( m_Procedure != NULL &&
                m_Procedure->IsSyntheticMethod() &&
                m_Procedure->GetContainer() != NULL &&
                m_Procedure->GetContainer()->IsAnonymousType() )
            {
                SetFlag(NameLookupFlags, NameSearchBindingAnonymousTypeFieldInSyntheticMethod);
            }

            if(LookingForAQueryOperator)
            {
                SetFlag(NameLookupFlags, NameSearchMethodsOnly);
            }

            Member =
                InterpretName
                (
                    MemberIdentifier->Name,
                    MemberLookup,
                    TypeParamToLookupInForMember,
                    NameLookupFlags,
                    AccessingInstanceType,
                    TextSpan,
                    MemberIsBad,
                    &GenericBindingContext,
                    GenericTypeArity,
                    &ignored
                );
            UpdateNameLookupGenericBindingContext(Member, TypeForGenericBinding, &GenericBindingContext);
        }
    }
    else if (Name->Opcode == ParseTree::Expression::AlreadyBoundSymbol)
    {
        AssertIfTrue(LookingForAQueryOperator);
        Member = Name->AsAlreadyBoundSymbol()->Symbol;
        MemberIdentifier = &ParserHelper::CreateIdentifierDescriptor(Member->PNamedRoot()->GetName());
    }

    if (!MemberIsBad)
    {
        // If early member binding succeeded, then attempt to generate an early bound call to the member
        if (Member)
        {
            STRING* MyDefaultInstanceBaseName = NULL;
            bool MangleName = false;
            if (!HasFlag(Flags, ExprAllowTypeReference) && Member->IsType())
            {
                if (Member->IsClass() &&
                    (MyDefaultInstanceBaseName = GetDefaultInstanceBaseNameForMyGroupMember(Member->PClass(), &MangleName)))
                {
                    Flags |= ExprAllowTypeReference;
                }
                else
                {
                    ReportSemanticError(
                        ERRID_TypeMemberAsExpression2,
                        TextSpan,
                        MemberIdentifier->Name,
                        BaseReferenceIsNamespace ? MemberLookup : BaseReferenceType);

                    return AllocateBadExpression(TextSpan);
                }
            }

            // 

            if (BaseReference &&
                TypeHelpers::IsValueType(BaseReferenceType) &&
                IsFieldOfMarshalByRefObject(BaseReference))
            {
                ReportSemanticError(
                    ERRID_FieldOfValueFieldOfMarshalByRef3,
                    TextSpan,
                    Member,
                    BaseReference,
                    BaseReference->AsSymbolReferenceExpression().BaseReference->ResultType);

                return AllocateBadExpression(TextSpan);
            }

            // Only fields or properties can be initialized in an Object initializer
            //
            if (HasFlag(Flags, ExprIsLHSOfObjectInitializer) &&
                (!((Member->IsVariable() && !Member->PVariable()->IsConstant()) ||
                        Member->IsProperty())))
            {
                ReportSemanticError(
                    ERRID_NonFieldPropertyAggrMemberInit1,
                    TextSpan,
                    MemberIdentifier->Name);

                return AllocateBadExpression(TextSpan);
            }

            // consider a base reference via a default instance.
            STRING* MyDefaultInstanceBaseNameForOriginalBase = NULL;
            bool MangleNameOrg = false;

            if (BaseReference == NULL && OriginalBaseReference &&
                !MyDefaultInstanceBaseName &&   //cannot be aplied twice
                (OriginalBaseReference->AsSymbolReferenceExpression().Symbol)->IsClass() && !HasFlag(Flags, ExprSuppressDefaultInstanceSynthesis ) &&
                // do not try to get a default instance for Form1.Type, Form1.sharedMember, Form1.Const, Form1.enum
                !(Member->IsType() || (Member->IsMember() && Member->PMember()->IsShared()) ||
                  (Member->IsVariable() && Member->PVariable()->IsConstant())) &&
                (MyDefaultInstanceBaseNameForOriginalBase =
                    GetDefaultInstanceBaseNameForMyGroupMember(OriginalBaseReference->AsSymbolReferenceExpression().Symbol->PClass(), &MangleNameOrg)))
            {
                BaseReference =
                    CheckForDefaultInstanceProperty(
                        TextSpan,
                        OriginalBaseReference,
                        MyDefaultInstanceBaseNameForOriginalBase,
                        ExprNoFlags,
                        MangleNameOrg);
            }

            // In the case of anonymous types, we may need to substitute the temp variable
            // that we associated with this property instead of binding to the property
            // variable. We do this because if this property that we bound to is declared
            // previously as a field on ths anonymous type, we use a temp for the anonymous
            // type field that we are referencing.

            bool ReplacedSymbol = false;

            if( m_AnonymousTypeBindingTable != NULL &&
                Member->IsProperty() &&
                Member->PProperty()->IsFromAnonymousType() &&
                ( BaseReference != NULL &&
                  BaseReference->bilop == SX_SYM &&
                  BaseReference->AsSymbolReferenceExpression().Symbol == m_AnonymousTypeBindingTable->m_BaseReference )
                )
            {
                // If we are evaluating a lambda, this is bad. We can't lift the anonymous type
                // instance today. So we need to emit an error. But we do this if we aren't in the
                // same lambda as the anonymous type. For example, the following is legal:
                // function() new with { .a = 1, .b = .a }
                // Dev10#515058: in a multiline lambda, it's possible that the first Resolving phase of the
                // anonymous type encountered a lambda, processed it once silently to infer result type (and
                // so reported the lifting error silently and marked the variable as bad but still inferred
                // Object return type), processed it a second time to interpret the lambda. And now we go on
                // to the Replacing phase, and still might encounter the problem, and still have to report it.
                //   Dim y = New With {.a = 1, .b = Function() : Dim z = .a : End Function}
                // Microsoft: consider making it so the lambda itself is marked as bad if it has bad
                // content. That would avoid us revisiting it a second time, and result in the same code-path
                // for single-line as for multi-line lambdas. This suggestion has been logged as Dev10#520603.

                if( m_InLambda && m_TemporaryManager != m_AnonymousTypeBindingTable->GetTemporaryManager() )
                {
                    ReportSemanticError(
                        ERRID_CannotLiftAnonymousType1,
                        TextSpan,
                        Member->PProperty()->GetName()
                        );
                    return AllocateBadExpression(TextSpan);
                }

                // Normally we ignore the resolving (done below) since we don't want to create temps until we
                // bind the type. However, we want to replace temps if we need to.

                if( m_AnonymousTypeBindingTable->m_Mode == AnonymousTypeBindingTable::Replacing )
                {
                    Result = m_AnonymousTypeBindingTable->GetTemp( Member->PProperty() );
                    ReplacedSymbol = true;

                    // Woah. If the result is NULL, it means that we bound to a previous symbol in the
                    // Anonymous type here, but not in the previous phase, where we should have constructed
                    // a temporary for this property.

                    ThrowIfNull( Result );
                }
            }

            if( !ReplacedSymbol )
            {
                Result =
                    ReferToSymbol(
                        TextSpan,
                        Member,
                        MemberIdentifier->TypeCharacter,
                        BaseReference,
                        GenericBindingContext,
                        Flags | ExprSuppressMeSynthesis);

                if (MyDefaultInstanceBaseName)
                {
                    // check for default instance on member as well
                    Result =
                        CheckForDefaultInstanceProperty(
                            TextSpan,
                            Result,
                            MyDefaultInstanceBaseName,
                            Flags,
                            MangleName);

                    if(Result == 0 || IsBad(Result) )
                    {
                        ReportSemanticError(
                            ERRID_TypeMemberAsExpression2,
                            TextSpan,
                            MemberIdentifier->Name,
                            BaseReferenceIsNamespace ? MemberLookup : BaseReferenceType);

                        return AllocateBadExpression(TextSpan);
                    }
                }
            }

            // Now that we have the symbol, check if we need to create a temp for the anonymous
            // type property. We need to create a temp if the property that we just bound to
            // is declared in this anonymous type.

            if( m_AnonymousTypeBindingTable != NULL &&
                Member->IsProperty() &&
                Member->PProperty()->IsFromAnonymousType() &&
                ( BaseReference != NULL &&
                  BaseReference->bilop == SX_SYM &&
                  BaseReference->AsSymbolReferenceExpression().Symbol == m_AnonymousTypeBindingTable->m_BaseReference )
                )
            {
                // Only create temps in the resolving state.

                if( m_AnonymousTypeBindingTable->m_Mode == AnonymousTypeBindingTable::Resolving )
                {

                    // If we are evaluating a lambda, this is bad. We can't lift the anonymous type
                    // instance today. So we need to emit an error. But we do this if we aren't in the
                    // same lambda as the anonymous type. For example, the following is legal:
                    // function() new with { .a = 1, .b = .a }

                    if( m_InLambda && m_TemporaryManager != m_AnonymousTypeBindingTable->GetTemporaryManager() )
                    {
                        ReportSemanticError(
                            ERRID_CannotLiftAnonymousType1,
                            TextSpan,
                            Member->PProperty()->GetName()
                            );

                        return AllocateBadExpression(TextSpan);
                    }

                    // When we are in the resolving mode, this means that we bound to a property
                    // that is in the anonymous type, so we need to create a temp for it if it
                    // does not already exist.

                    VSASSERT(
                        m_AnonymousTypeBindingTable->DummyExists( Member->PProperty() ),
                        "The anonymous type member is not in the binding table?"
                        );

                    if( !m_AnonymousTypeBindingTable->HasTemp( Member->PProperty() ) )
                    {
                        Location loc = TextSpan;
                        Variable* Temp = m_AnonymousTypeBindingTable->GetTemporaryManager()->AllocateShortLivedTemporary(
                            Result->ResultType,
                            &loc
                            );

                        ILTree::Expression* ExprTemp= ReferToSymbol(
                            TextSpan,
                            Temp,
                            chType_NONE,
                            NULL,
                            GenericBindingContext,
                            Flags | ExprSuppressMeSynthesis
                            );

                        m_AnonymousTypeBindingTable->AddTemp( Member->PProperty(), ExprTemp );
                    }
                }
            }
        }
        else if (Opcode == ParseTree::Expression::BangQualified)
        {
            VSASSERT(BaseReference, "NULL unexpected as base expression for '!' qualified expression!!!");
            VSASSERT(MemberIdentifier, "Bang must be followed by constant identifier");

            // Dictionary access is implemented by:
            //
            //     x!y    ==> x.default("y")

            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    TextSpan);

                return AllocateBadExpression(TextSpan);
            }

            ILTree::Expression *PropertyReference = NULL;

            if (TypeHelpers::IsRootObjectType(BaseReference->ResultType))
            {
                // Option Strict disallows late binding.
                if (m_UsingOptionTypeStrict)
                {
                    ReportSemanticError(ERRID_StrictDisallowsLateBinding, TextSpan);
                    return AllocateBadExpression(TextSpan);
                }

                // Starlite Library doesn't support late binding, lower precedence than Option Strict
                if (m_CompilerHost->IsStarliteHost())
                {
                    ReportSemanticError(
                        ERRID_StarliteDisallowsLateBinding,
                        TextSpan);

                    return AllocateBadExpression(TextSpan);
                }

                // Warnings have lower precedence than Option Strict and Starlite errors.
                if (WarnOptionStrict())
                {
                    ReportSemanticError(WRNID_LateBindingResolution, TextSpan);
                }

                PropertyReference =
                    AllocateExpression(
                        SX_VARINDEX,
                        GetFXSymbolProvider()->GetObjectType(),
                        BaseReference,
                        NULL,
                        TextSpan);
                SetResultType(PropertyReference, GetFXSymbolProvider()->GetObjectType());

                PropertyReference =
                    AllocateExpression(
                        SX_LATE_REFERENCE,
                        GetFXSymbolProvider()->GetObjectType(),
                        PropertyReference,
                        NULL,
                        TextSpan);
            }
            else
            {
                if (!TypeHelpers::IsClassInterfaceRecordOrGenericParamType(BaseReference->ResultType))
                {
                    ReportSemanticError(
                        ERRID_QualNotObjectRecord1,
                        BaseReference->Loc,
                        BaseReference->ResultType);

                    return AllocateBadExpression(TextSpan);
                }

                PropertyReference =
                    AccessDefaultProperty(
                        TextSpan,
                        BaseReference,
                        MemberIdentifier->TypeCharacter,
                        ExprPropagatePropertyReference);

                if (IsBad(PropertyReference))
                {
                    return AllocateBadExpression(TextSpan);
                }
                else if (!IsPropertyReference(PropertyReference))
                {
                    ReportSemanticError(
                        ERRID_DefaultMemberNotProperty1,
                        PropertyReference->Loc,
                        PropertyReference);

                    return AllocateBadExpression(TextSpan);
                }
            }

            ILTree::Expression *NameArgument =
                ProduceStringConstantExpression(
                    MemberIdentifier->Name,
                    wcslen(MemberIdentifier->Name),
                    TextSpan
                    IDE_ARG(0));

            NameArgument =
                AllocateExpression(SX_ARG, TypeHelpers::GetVoidType(), NameArgument, NULL, NameArgument->Loc);

            ExpressionList *NameArgumentList =
                AllocateExpression(SX_LIST, TypeHelpers::GetVoidType(), NameArgument, NULL, NameArgument->Loc);

            PropertyReference->AsPropertyReferenceExpression().Right = NameArgumentList;

            // If this dictionary access is the operand of another
            // indexing, allowing this property reference to propagate leads
            // to confusion in processing the parent indexing. Therefore, interpret
            // this one.

            if (HasFlag(Flags, ExprIsExplicitCallTarget))
            {
                PropertyReference = FetchFromProperty(PropertyReference);
            }

            Result = PropertyReference;
        }

        // Check for Xml member binding (including the special-case of the extension Value property)
        else if(BaseReference && // 
                (Opcode != ParseTree::Expression::DotQualified ||
                (StringPool::IsEqual(MemberIdentifier->Name, STRING_CONST(m_Compiler, Value)) &&
                 m_XmlSymbols.GetXElement() &&
                 TypeHelpers::IsCompatibleWithGenericEnumerableType(
                     BaseReference->ResultType,
                     m_XmlSymbols.GetXElement(),
                     m_SymbolCreator,
                     m_CompilerHost)))
                 )
        {
            if (!CheckXmlFeaturesAllowed(TextSpan, Flags))
            {
                return AllocateBadExpression(TextSpan);
            }

            // Late binding not supported
            if (TypeHelpers::IsRootObjectType(BaseReference->ResultType))
            {
                ReportSemanticError(ERRID_NoXmlAxesLateBinding, TextSpan);
                return AllocateBadExpression(TextSpan);
            }

            // Do Xml member binding
            ParserHelper PH(&m_TreeStorage, TextSpan);
            ILTree::Expression * CallTarget;
            bool IsExtension = true;

            switch (Opcode)
            {
                case ParseTree::Expression::XmlElementsQualified:
                case ParseTree::Expression::XmlDescendantsQualified:
                {
                    // Only types compatible with XContainer and IEnumerable(Of XContainer) support Elements and Descendants axes
                    if (TypeHelpers::IsCompatibleWithTypeOrGenericEnumerableType(BaseReference->ResultType, m_XmlSymbols.GetXContainer(), m_SymbolCreator, GetCompilerHost()))
                    {
                        // Determine whether extension versions of axes will be called
                        if (IsOrInheritsFromOrImplements(BaseReference->ResultType, m_XmlSymbols.GetXContainer()))
                        {
                            IsExtension = false;
                        }

                        // Create the Elements or Descendants target for the call
                        CallTarget = ReferToProcByName(
                                        Name->TextSpan,
                                        IsExtension ? m_XmlSymbols.GetXmlExtensions() : m_XmlSymbols.GetXContainer(),
                                        Opcode == ParseTree::Expression::XmlElementsQualified ?
                                            STRING_CONST(m_Compiler, XmlElementsMethod) :
                                            STRING_CONST(m_Compiler, XmlDescendantsMethod),
                                        IsExtension ? NULL : BaseReference,
                                        ExprNoFlags);
                    }
                    else
                    {
                        ReportSemanticError(Opcode == ParseTree::Expression::XmlElementsQualified ? ERRID_TypeDisallowsElements : ERRID_TypeDisallowsDescendants, TextSpan, BaseReference->ResultType);
                        return AllocateBadExpression(TextSpan);
                    }
                    break;
                }

                case ParseTree::Expression::XmlAttributeQualified:
                    // Only XElement nodes support the AttributeValue methods
                    if (TypeHelpers::IsCompatibleWithTypeOrGenericEnumerableType(BaseReference->ResultType, m_XmlSymbols.GetXElement(), m_SymbolCreator, GetCompilerHost()))
                    {
                        // Call the Xml helper AttributeValue method, passing the BaseReference and identifier
                        Procedure *Method = GetXmlHelperMethod(STRING_CONST(m_Compiler, XmlAttributeValueMethod));
                        CallTarget = ReferToSymbol(Name->TextSpan, Method, chType_NONE, NULL, NULL, ExprIsExplicitCallTarget);
                    }
                    else
                    {
                        ReportSemanticError(ERRID_TypeDisallowsAttributes, TextSpan, BaseReference->ResultType);
                        return AllocateBadExpression(TextSpan);
                    }
                    break;

                default:
                {
                    // Call the Xml helper Value method, passing the BaseReference
                    Procedure *Method = GetXmlHelperMethod(STRING_CONST(m_Compiler, Value));

                    // Create an SX_EXTENSIONCALL
                    ExtensionCallLookupResult *ExtensionResult = m_SymbolCreator.GetExtensionCallLookupResult();
                    ExtensionResult->AddProcedure(Method, 0, NULL);

                    Result = ReferToExtensionMethod(TextSpan, ExtensionResult, BaseReference, Flags, chType_NONE);

                    // Mark the expression as an extension call so that IDE will display it nicely
                    SetFlag32(Result, SXF_CALL_WAS_EXTENSION_CALL);

                    return Result;
                }
            }

            // Instance method has 1 argument, other extension methods have 2 arguments
            Result = BindArgsAndInterpretCallExpressionWithNoCopyOut(
                        TextSpan,
                        CallTarget,
                        chType_NONE,
                        IsExtension ?
                            PH.CreateArgList(Name->TextSpan, 2, PH.CreateBoundExpression(BaseReference), Name) :
                            PH.CreateArgList(Name->TextSpan, 1, Name),
                        Flags,
                        OvrldNoFlags,
                        NULL);

            if (IsExtension)
            {
                // Mark the expression as an extension call so that IDE will display it nicely
                SetFlag32(Result, SXF_CALL_WAS_EXTENSION_CALL);
            }
        }

        // Check for late binding to a member of Object or an extensible class.
        // The test must be on the type of the base reference, not on
        // BaseReferenceType, because BaseReferenceType has been chased through to
        // an object symbol.

        else if (BaseReference &&
                 !HasFlag(Flags, ExprSuppressLateBinding) &&
                 AllowsLateBinding(BaseReference))
        {
            if (HasFlag(Flags, ExprMustBeConstant))
            {
                ReportSemanticError(
                    ERRID_RequiredConstExpr,
                    TextSpan);

                return AllocateBadExpression(TextSpan);
            }

            // Starlite Library doesn't support late binding, lower precedence than Option Strict
            if (m_CompilerHost->IsStarliteHost())
            {
                ReportSemanticError(
                    ERRID_StarliteDisallowsLateBinding,
                    TextSpan);
                return AllocateBadExpression(TextSpan);
            }

            // Option Strict disallows late binding.
            if ( m_UsingOptionTypeStrict )
            {
                ReportSemanticError(ERRID_StrictDisallowsLateBinding, TextSpan);
                return AllocateBadExpression(TextSpan);
            }

            // Warnings have lower precedence than Option Strict
            if (WarnOptionStrict())
            {
                ReportSemanticError(WRNID_LateBindingResolution, TextSpan);
            }

            bool BaseIsRValue = !HasFlag32(BaseReference, SXF_LVALUE);

            BaseReference = Convert(
                MakeRValue(BaseReference),
                GetFXSymbolProvider()->GetObjectType(),
                ExprNoFlags,
                ConversionWidening);

            Result =
                AllocateExpression(
                    SX_LATE,
                    GetFXSymbolProvider()->GetObjectType(),
                    BaseReference,
                    NULL,
                    TextSpan);

            AssertIfNull(MemberIdentifier);

            Result->AsLateBoundExpression().LateIdentifier = ProduceStringConstantExpression(
                MemberIdentifier->Name,
                StringPool::StringLength(MemberIdentifier->Name),
                Name->TextSpan
                IDE_ARG(0));

            if (BaseIsRValue)
            {
                SetFlag32(Result, SXF_LATE_RVALUE_BASE);
            }

            Result =
                AllocateExpression(
                    SX_LATE_REFERENCE,
                    GetFXSymbolProvider()->GetObjectType(),
                    Result,
                    NULL,
                    TextSpan);

            Result->AsPropertyReferenceExpression().TypeCharacter = MemberIdentifier->TypeCharacter;

            if (!HasFlag(Flags, ExprPropagatePropertyReference))
            {
                Result =
                    InterpretLateBoundExpression(
                        TextSpan,
                        Result->AsPropertyReferenceExpression(),
                        Result->AsPropertyReferenceExpression().Right,
                        Flags);
            }
        }

        else
        {
            // Binding could not be performed, so report error
            STRING *NameOfUnnamedNamespace = STRING_CONST(m_Compiler, EmptyString);
            VSASSERT(MemberIdentifier, "Dynamic identifiers not supported--identifier should have been constant here.");

            if (BaseReferenceIsNamespace && StringPool::IsEqual(MemberLookup->GetName(), NameOfUnnamedNamespace))
            {
                ReportSemanticError(
                    ERRID_NameNotMember2,
                    TextSpan,
                    MemberIdentifier->Name,
                    STRING_CONST( m_Compiler, UnnamedNamespaceErrName) );
            }
            else if (BaseReferenceType->DigThroughAlias()->IsAnonymousType())
            {
                StringBuffer sbTemp;
                ResLoadStringRepl(STRID_AnonymousType, &sbTemp);

                ReportSemanticError(
                    ERRID_NameNotMemberOfAnonymousType2,
                    TextSpan,
                    MemberIdentifier->Name,
                    sbTemp.GetString());
            }
            else if (TypeHelpers::IsEmbeddedLocalType(BaseReferenceType->DigThroughAlias()))
            {
                // This branch is added for 
                ReportSemanticError(
                    ERRID_MemberNotFoundForNoPia,
                    TextSpan,
                    MemberIdentifier->Name,
                    BaseReferenceIsNamespace ? MemberLookup : BaseReferenceType);
            }
            else
            {
                ReportSemanticError(
                    ERRID_NameNotMember2,
                    TextSpan,
                    MemberIdentifier->Name,
                    BaseReferenceIsNamespace ? MemberLookup : BaseReferenceType);
            }
            MemberIsBad = true;
        }
    }

    if (MemberIsBad)
    {
        //An extension call lookup result will never
        //be marked as bad, so if MemberIsBad is true, then
        //we know we must have a named root.
        Declaration * pMemberAsNamed = Member->PNamedRoot();

        // Intellisense expects to receive a qualified reference
        // to a bad symbol. The base reference should be the
        // original base reference, whether it refers to an
        // object, type, or namespace.

        if (m_PreserveExtraSemanticInformation)
        {
            Declaration *BadSymbol = NULL;
            BadSymbol =
                m_SymbolCreator.GetBadNamedRoot(
                    MemberIdentifier ? MemberIdentifier->Name : NULL,
                    NULL,
                    DECLF_Public,
                    BINDSPACE_Normal,
                    0,
                    NULL,
                    NULL);
            if (Member && BadSymbol)
            {
                // NameIsBad could be set for a correctsymbol, ie a not accessible symbol
                // GetBadNameSpace() would hit an assert otherwise, we want to keep.
                if(Member->IsBad())
                {
                    BadSymbol->SetBadNameSpace(pMemberAsNamed->GetBadNameSpace());
                    BadSymbol->SetBadExtra(pMemberAsNamed->GetBadExtra());
                }

            }

            return
                MakeBad(
                    AllocateSymbolReference(
                        BadSymbol,
                        TypeHelpers::GetVoidType(),
                        OriginalBaseReference,
                        TextSpan));
        }
        else
        {
            return AllocateBadExpression(TextSpan);
        }
    }

    return Result;
}

bool
Semantics::AllowsLateBinding
(
    ILTree::Expression *BaseReference
)
{
    // No late binding for "MyBase.Name".
    if (BaseReference->bilop == SX_SYM && HasFlag32(BaseReference, SXF_SYM_MYBASE))
    {
        return false;
    }

    // typeof(object) allows late binding
    if (TypeHelpers::IsRootObjectType(BaseReference->ResultType))
    {
        return true;
    }

    // Extensible interfaces allow late binding
    if (TypeHelpers::IsInterfaceType(BaseReference->ResultType))
    {
        if (BaseReference->ResultType->PInterface()->IsDispinterface() ||
            BaseReference->ResultType->PInterface()->IsExtensible())
        {
            return true;
        }
    }

    return false;
}

ILTree::Expression *
Semantics::CreateConstructedInstance
(
    Type *TypeOfInstance,
    const Location &TypeTextSpan,
    const Location &TextSpan,
    ParseTree::ArgumentList *UnboundArguments,
    ExpressionFlags Flags
)
{
    ILTree::Expression *Result = NULL;
    bool ReportErrors = m_ReportErrors;


   // If the class to be constructed is a delegate, and there
    // is a single argument to New that is an AddressOf a
    // dot-qualified expression, then this construction requires
    // special treatment. The argument expands to two arguments,
    // and the method to which the delegate instance will be bound
    // is selected by finding one with the same signature as the
    // delegate's Invoke method.
    //
    // All of this is accomplished by interpreting the AddressOf
    // expression with the right type context.'
    //
    // The same holds true for Lambda's

    if (TypeHelpers::IsDelegateType(TypeOfInstance))
    {
        if (ArgumentsAllowedAsDelegateConstructorArguments(UnboundArguments))
        {
            Result =
                InterpretExpressionWithTargetType(
                    UnboundArguments->Element->Value,
                    Flags | ExprForceRValue | ExprCreateDelegateInstance,
                    TypeOfInstance);

            Result->Loc = TextSpan;
            return Result;
        }

        else
        {
            ReportSemanticError(
                ERRID_NoDirectDelegateConstruction1,
                UnboundArguments ? UnboundArguments->TextSpan : TextSpan,
                TypeOfInstance);

            // Disable reporting other (bogus) errors for the construction.
            // (Semantic analysis of the construction must occur
            // so that Intellisense can provide tips.)
            m_ReportErrors = false;
        }
    }

    bool SomeArgumentsBad = false;
    ExpressionList *BoundArguments =
        InterpretArgumentList(
            UnboundArguments,
            SomeArgumentsBad,
            Flags & ExprArgumentsMustBeConstant);

    Result =
        CreateConstructedInstance(
            TypeOfInstance,
            TypeTextSpan,
            TextSpan,
            BoundArguments,
            SomeArgumentsBad,
            Flags);

    m_ReportErrors = ReportErrors;

    return Result;
}

ILTree::Expression *
Semantics::CreateConstructedInstance
(
    Type *TypeOfInstance,
    const Location &TypeTextSpan,
    const Location &TextSpan,
    ExpressionList *BoundArguments,
    bool SomeArgumentsBad,
    ExpressionFlags Flags
)
{
    unsigned ErrorId = CheckConstraintsOnNew(TypeOfInstance);

    if (ErrorId != 0)
    {
        ReportSemanticError(
            ErrorId,
            TypeTextSpan,
            TypeOfInstance);

        return AllocateBadExpression(TextSpan);
    }

    ILTree::Expression *ConstructorCall = NULL;
    bool ConstructorCallIsBad = false;
    ExpressionList *CopyOutArguments = NULL;
    ILTree::Expression *Result = NULL;

    if (TypeHelpers::IsGenericParameter(TypeOfInstance))
    {
        VSASSERT(TypeOfInstance->PGenericParam()->CanBeInstantiated(), "Non-new constrained type param unexpected!!!");

        if (BoundArguments)
        {
            // Arguments cannot be passed to a 'New' used on a type parameter.

            ReportSemanticError(
                ERRID_NewArgsDisallowedForTypeParam,
                BoundArguments->Loc);

            return AllocateBadExpression(TextSpan);
        }

        ILTree::NewExpression *New =
            &AllocateExpression(
                SX_NEW,
                TypeOfInstance,
                TextSpan)->AsNewExpression();
        New->Class = TypeOfInstance;

        return New;
    }
    //
    // Creations of structures with no arguments use the built-in, nondeclarable,
    // uncallable parameterless constructor. Other creations call a constructor.
    //
    else if (TypeHelpers::IsReferenceType(TypeOfInstance) || BoundArguments)
    {
        // To facilitate COM2 interop, interfaces are decorated with a CoClass
        // attribute that points at the "real" type that a coclass is. So you can
        // say "New Recordset" where Recordset is an interface that has a CoClass
        // attribute that points at RecordsetClass class. So do the indirection here.
        if (TypeHelpers::IsInterfaceType(TypeOfInstance))
        {
            WCHAR *CoClassName;
            NorlsAllocator Scratch(NORLSLOC);

            if (!TypeOfInstance->GetPWellKnownAttrVals()->GetCoClassData(&CoClassName))
            {
                VSFAIL("How did this type get here?");
            }

            unsigned NameCount = m_Compiler->CountQualifiedNames(CoClassName);
            STRING **Names = (STRING **)Scratch.Alloc(VBMath::Multiply(
                sizeof(STRING *), 
                NameCount));
            bool IsBadCoClassName = false;
            Scope *Lookup;
            Declaration *ConstructedType;

            m_Compiler->SplitQualifiedName(CoClassName, NameCount, Names);
            Lookup = m_Compiler->GetUnnamedNamespace(m_Project)->GetHash();

            //EXTMET - 
            ConstructedType =
                Semantics::EnsureNamedRoot
                (
                    Semantics::InterpretQualifiedName
                    (
                        Names,
                        NameCount,
                        NULL,
                        NULL,
                        Lookup,
                        NameSearchUnnamedNamespace | NameSearchIgnoreImports | NameSearchIgnoreExtensionMethods,
                        TextSpan,
                        NULL,
                        m_Compiler,
                        m_CompilerHost,
                        m_CompilationCaches,
                        NULL,
                        true,                              // perform obsolete checks
                        IsBadCoClassName
                    )
                );

            if (IsBadCoClassName && ConstructedType && !ConstructedType->IsGenericBadNamedRoot() &&
                !IsAccessible(ConstructedType, NULL, Lookup->GetContainer()))
            {
                ReportSemanticError(
                    ERRID_InAccessibleCoClass3,
                    TextSpan,
                    ConstructedType,
                    TypeOfInstance,
                    ConstructedType->GetAccess());

                return AllocateBadExpression(TextSpan);
            }

            // Including IsBadCoClassName too here as a fall back for any other kind of badness
            // like obsoleteness etc., so that the user gets atleast the default error instead
            // of wrong compiling without any errors.
            //
            if (IsBadCoClassName || !ConstructedType || ConstructedType->IsBad() || !ConstructedType->IsClass())
            {
                ReportSemanticError(
                    ERRID_CoClassMissing2,
                    TextSpan,
                    CoClassName,
                    TypeOfInstance);

                return AllocateBadExpression(TextSpan);
            }
            // Is the type we are about to create a NoPIA class? If so, then instead of actually
            // instantiating the .NET class, we should call Activator.CreateInstance with its GUID.
            else if (TypeHelpers::IsEmbeddableInteropType(TypeOfInstance))
            {
                return CreateInstanceComInteropNoPIA(TypeOfInstance, ConstructedType, TextSpan);
            }
            else
            {
                TypeOfInstance = ConstructedType;
            }
        }
        
        if (TypeHelpers::IsEmbeddableInteropType(TypeOfInstance))
        {
            ReportSemanticError(
                ERRID_NewCoClassNoPIA, 
                TextSpan, 
                TypeOfInstance->PNamedRoot()->GetName(), 
                TypeOfInstance);
        }

        Procedure *Constructor = TypeOfInstance->PClass()->GetFirstInstanceConstructor(m_Compiler);

        if (Constructor == NULL)
        {
            ReportSemanticError(
                ERRID_ConstructorNotFound1,
                TextSpan,
                TypeOfInstance);

            return AllocateBadExpression(TextSpan);
        }

        // Since we never resolve the name, we have to check accessibility here.
        // CheckAccessibility won't actually do anything if the constructor is
        // overloaded, but InterpretCallExpression will take care of that later.
        {
            bool ResultIsBad = false;

            CheckAccessibility(
                Constructor,
                TypeOfInstance->IsGenericTypeBinding() ?
                    TypeOfInstance->PGenericTypeBinding() :
                    NULL,
                TextSpan,
                NameNoFlags,
                ContainingClass(), // Dev10 #789809 For constructor we should use type of containing class as the type of the instance, similar to Semantics::InitializeConstructedVariable().
                ResultIsBad);

            if (ResultIsBad)
            {
                return AllocateBadExpression(TextSpan);
            }
        }

        VSASSERT(TypeOfInstance->PClass()->AreBaseAndImplementsLoaded(), "must load base and implements first!");

        ILTree::Expression *ConstructorReference =
            ReferToSymbol(
                TypeTextSpan,
                Constructor,
                chType_NONE,
                NULL,
                DeriveGenericBindingForMemberReference(TypeOfInstance, Constructor, m_SymbolCreator, m_CompilerHost),
                ExprIsExplicitCallTarget | ExprIsConstructorCall);

        //EXTMET - 
        ConstructorCall =
            InterpretCallExpression(
                TextSpan,
                ConstructorReference,
                chType_NONE,
                BoundArguments,
                CopyOutArguments,
                SomeArgumentsBad,
                ExprResultNotNeeded | ExprIsConstructorCall,
                OvrldNoFlags,
                NULL);

        if (IsBad(ConstructorCall))
        {
            // Don't just return a bad expression here, because
            // Intellisense needs access to a New expression.
            ConstructorCallIsBad = true;
        }

    }

    // XMLGen doesn't like SX_SEQ_OP2s, so we don't generate them when the client is XMLGen.
    if (TypeHelpers::IsValueType(TypeOfInstance) && !m_IsGeneratingXML)
    {
        Variable *ResultTemporary = AllocateResultTemporary(TypeOfInstance);

        Result =
            MakeRValue(
                AllocateSymbolReference(
                    ResultTemporary,
                    TypeOfInstance,
                    NULL,
                    TextSpan));

        ILTree::Expression *ReferenceToInit =
            AllocateSymbolReference(
                ResultTemporary,
                TypeOfInstance,
                NULL,
                TextSpan);
        ILTree::Expression *Init = ConstructorCall;

        // Attach the constructor call. If there is no constructor call
        // to make, attach a default initialization. (The temporary is not
        // necessarily in a clean state--it may have been used to hold a value
        // previously in this method.)

        if (ConstructorCall)
        {
            if (!ConstructorCallIsBad)
            {
                ConstructorCall->AsCallExpression().MeArgument = MakeAddress(ReferenceToInit, true);
            }
        }
        else
        {
            Init =
                AllocateExpression(
                    SX_INIT_STRUCTURE,
                    TypeHelpers::GetVoidType(),
                    TextSpan);
            Init->AsInitStructureExpression().StructureReference = MakeAddress(ReferenceToInit, true);
            Init->AsInitStructureExpression().StructureType = TypeOfInstance;
        }

        Result =
            AllocateExpression(
                SX_SEQ_OP2,
                TypeOfInstance,
                Init,
                Result,
                TextSpan);
    }
    else
    {
        ILTree::NewExpression *New =
            &AllocateExpression(
                SX_NEW,
                TypeOfInstance,
                TextSpan)->AsNewExpression();
        New->Class = TypeOfInstance;
        New->ConstructorCall = ConstructorCall;

        Result = New;
    }

    if (ConstructorCallIsBad)
    {
        return MakeBad(Result);
    }

    // If the constructor call includes arguments passed to ByRef
    // parameters using temporaries, the assignments back from the
    // temporaries must be part of a sequence enclosing the New tree--
    // code generation expects only the call as the operand to the
    // New operator.

    Result = AppendCopyOutArguments(Result, CopyOutArguments, Flags);

    return Result;
}

ILTree::Expression *Semantics::CreateInstanceComInteropNoPIA
(
    Type *TypeOfInstance,   // this is the interface (which we will cast the result to)
    Type *ConstructedType,  // this is the co-class (which will have the guid)
    const Location &loc
)
{
    BCSYM_NamedRoot *NamedCoClass = ConstructedType->PNamedRoot();
    // Create an instance of a COM interop type in No-PIA mode.
    // Instead of instantiating the .NET class, we will construct a call
    // to System.Activator.CreateInstance. The parameter will be a TypeID 
    // obtained from the CLSID attached to our input type.
    WCHAR *guid = NULL;
    if (!NamedCoClass->GetPWellKnownAttrVals()->GetGuidData(&guid))
    {
        ReportSemanticError(
                ERRID_NoPIAAttributeMissing2, 
                loc, 
                NamedCoClass->GetName(),
                GUIDATTRIBUTE_NAME);
    }

    // I don't know if this is the most elegant way to do it, but it seems like less
    // work than individually constructing all the little bits of ILTree objects.
    // We are just going to macro up the relevant source code, call
    // ParseTreeService::ParseOneExpression, then call InterpretExpression to turn
    // it into an output ILTree.
    //
    // Use System.Runtime.InteropServices.Marshal.GetTypeFromCLSID() to obtain the type for the CLSID, if available.
    // Fall back to using System.Type.GetTypeFromCLSID()
    bool fUseMarshalGetTypeFromCLSID = false;
    BCSYM_Class* marshalClass = GetFXSymbolProvider()->GetType(FX::MarshalType)->PClass();
    if (marshalClass != NULL)
    {
        BCSYM_NamedRoot* getTypeFromCLSIDProc = marshalClass->SimpleBind(NULL, GetCompiler()->AddString(L"GetTypeFromCLSID"));
        if (getTypeFromCLSIDProc != NULL)
        {
            VSASSERT(getTypeFromCLSIDProc->IsProc(), "Unexpected: Found System.Runtime.InteropServices.Marshal.GetTypeFromCLSID() which is not a proc?");
            fUseMarshalGetTypeFromCLSID = true;
        }
    }
    StringBuffer factoryText;
    factoryText.AppendString(L"DirectCast(");
    factoryText.AppendString(L"Global.System.Activator.CreateInstance(");
    if (fUseMarshalGetTypeFromCLSID)
    {
        factoryText.AppendString(L"Global.System.Runtime.InteropServices.Marshal.GetTypeFromCLSID(New Global.System.Guid(\"");
    }
    else
    {
        factoryText.AppendString(L"Global.System.Type.GetTypeFromCLSID(New Global.System.Guid(\"");
    }
    factoryText.AppendString(guid);
    factoryText.AppendString(L"\")))");
    factoryText.AppendString(L",");
    STRING *bcns = TypeOfInstance->PNamedRoot()->GetNameSpace();
    if (bcns && bcns[0])
    {
        factoryText.AppendString(bcns);
        factoryText.AppendString(L".");
    }
    factoryText.AppendString(TypeOfInstance->PNamedRoot()->GetName());
    factoryText.AppendString(L")");
    
    Parser factoryParser(
        &m_TreeStorage,
        m_Compiler, 
        m_CompilerHost,
        false,
        m_Project->GetCompilingLanguageVersion()
        );

    Scanner
        Scanner(
        m_Compiler,
        factoryText.GetString(),
        factoryText.GetStringLength(),
        0,
        loc.m_lBegLine,
        loc.m_lBegColumn);

    ParseTree::Expression *instanceParseTree;
    bool ErrorInConstructRet = false;
    factoryParser.
        ParseOneExpression(
            &Scanner,
            NULL,
            &instanceParseTree,
            &ErrorInConstructRet);

    AssertIfTrue(ErrorInConstructRet);
    AssertIfNull(instanceParseTree);

    // Evaluate the parse tree and return the resulting ILTree.
    ExpressionFlags OperandFlags = 0;
    ILTree::Expression* Result = InterpretExpression(
        instanceParseTree,
        OperandFlags);

    return Result;
}

ExpressionList *
Semantics::InterpretArraySizeList
(
    ParseTree::ArrayDimList *Dimensions,
    ExpressionFlags Flags,
    bool &SomeDimensionsBad
)
{
    // Interpret the dimension sizes, and count the number of dimensions
    // If there are no specified sizes, the number of dimensions is 1.
    unsigned DimensionCount = 1;
    ExpressionList *DimensionSizes = NULL;
    ExpressionList **SizesTarget = &DimensionSizes;

    SomeDimensionsBad = false;

    for (ParseTree::ArrayDimList *DimsToCount = Dimensions;
        DimsToCount;
        DimsToCount = DimsToCount->Next)
    {
        DimensionCount++;

        ParseTree::Expression *Size = DimsToCount->Element->upperBound;

        ILTree::Expression *BoundSize =
            InterpretExpressionWithTargetType(
                Size,
                ExprScalarValueFlags | (Flags & ExprMustBeConstant),
                GetFXSymbolProvider()->GetIntegerType());

        if (IsBad(BoundSize))
        {
            SomeDimensionsBad = true;
        }
        else if (BoundSize->bilop == SX_CNS_INT)
        {
            BoundSize->AsIntegralConstantExpression().Value++;
            // 
            if (BoundSize->AsIntegralConstantExpression().Value < 0)
            {
                ReportSemanticError(
                    ERRID_NegativeArraySize,
                    BoundSize->Loc);
            }
        }
        else
        {
            ILTree::Expression *One =
                ProduceConstantExpression(1, BoundSize->Loc, GetFXSymbolProvider()->GetIntegerType() IDE_ARG(0));

            BoundSize =
                AllocateExpression(
                    SX_ADD,
                    GetFXSymbolProvider()->GetIntegerType(),
                    BoundSize,
                    One,
                    BoundSize->Loc);
        }

        ExpressionList *ListElement =
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                BoundSize,
                NULL,
                Size->TextSpan);

        *SizesTarget = ListElement;
        SizesTarget = &ListElement->AsExpressionWithChildren().Right;
    }

    return DimensionSizes;
}

ExpressionList *
Semantics::InterpretArrayInitializerList
(
    ParseTree::BracedInitializerList *Input,
    ExpressionFlags Flags
)
{
    // An empty aggregate initializer becomes a list with one (empty) element.

    if (Input->InitialValues == NULL)
    {
        return
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                NULL,
                NULL,
                Input->TextSpan);
    }

    ExpressionList *Result = NULL;
    ExpressionList **ListTarget = &Result;

    bool SomeOperandsBad = false;

    for (ParseTree::InitializerList *Initializers = Input->InitialValues;
         Initializers;
         Initializers = Initializers->Next)
    {
        ParseTree::Initializer *Operand = Initializers->Element;
        ILTree::Expression *BoundOperand = NULL;

        if (Operand)
        {
            if (ParseTree::Initializer::Expression == Operand->Opcode)
            {
                ParseTree::Expression *Value = Operand->AsExpression()->Value;
                AssertIfNull(Value);

                if (ParseTree::Expression::ArrayInitializer == Value->Opcode)
                {
                    // Multi-dimensional array
                    BoundOperand =
                        InterpretArrayInitializerList(
                            Value->AsArrayInitializer()->Elements,
                            Flags);
                }
                else
                {
                    BoundOperand =
                        InterpretExpression(
                            Value,
                            Flags | ExprDontInferResultType); // We don't want to infer a result type because a few lines below we will convert.
                }
            }
            else
            {
                VSFAIL("Surprising array element initializer");
            }

            if (IsBad(BoundOperand))
            {
                SomeOperandsBad = true;
            }
        }

        ExpressionList *ListElement =
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                BoundOperand,
                NULL,
                Initializers->TextSpan);

        *ListTarget = ListElement;
        ListTarget = &ListElement->AsExpressionWithChildren().Right;
    }

    if (SomeOperandsBad)
    {
        MakeBad(Result);
    }

    return Result;
}

ILTree::Expression *
Semantics::InitializeArray
(
    ExpressionList *Initializer,
    ArrayType *ResultType,
    ExpressionList *DimensionSizes,
    const Location &TextSpan
)
{
    return
        InitializeArray(
            Initializer,
            ResultType,
            DimensionSizes,
            TextSpan,
            NULL);
}

ILTree::Expression *
Semantics::InitializeArray
(
    ExpressionList *Initializer,
    ArrayType *ResultType,
    ExpressionList *DimensionSizes,
    const Location &TextSpan,
    unsigned StorageIndices[]
)
{
    Variable *ArrayTemporary = NULL;
    return
        InitializeArray(
            Initializer,
            ResultType,
            DimensionSizes,
            TextSpan,
            StorageIndices,
            ArrayTemporary);
}

ILTree::Expression *
Semantics::InitializeArray
(
    ExpressionList *Initializer,
    ArrayType *ResultType,
    ExpressionList *DimensionSizes,
    const Location &TextSpan,
    unsigned StorageIndices[],
    Variable *&ArrayTemporary
)
{
    unsigned DimensionCount = ResultType->GetRank();

    DIMCOUNTS DimensionCountsScratch[5];
    DIMCOUNTS *DimensionCounts = DimensionCountsScratch;
    if (DimensionCount > 5)
    {
        DimensionCounts = new(m_TreeStorage) DIMCOUNTS[DimensionCount];
    }

    // For any dimension with a size not specified in the input
    // dimension sizes, fix the size of that dimension as the
    // length of the first initializer list in that dimension.

    // Create a list of the sizes of each dimension--
    // this is part of the tree for the array creation.
    // If a DimensionSizes list has been provided, use it,
    // filling in any missing dimensions.

    ExpressionList **DimensionTarget = &DimensionSizes;

    ExpressionList *DimensionInitializer = Initializer;

    unsigned DimensionIndex;
    for (DimensionIndex = 0;
         DimensionIndex < DimensionCount;
         DimensionIndex++)
    {
        unsigned LengthInThisDimension = 0;
        bool ThisDimensionIsNotConstant = false;

        // Test to see if the size in this dimension is supplied,
        // or if it needs to be computed.

        if ((*DimensionTarget) && (*DimensionTarget)->AsExpressionWithChildren().Left)
        {
            if ((*DimensionTarget)->AsExpressionWithChildren().Left->bilop == SX_CNS_INT)
            {
                LengthInThisDimension = (unsigned)(*DimensionTarget)->AsExpressionWithChildren().Left->AsIntegralConstantExpression().Value;
            }
            else
            {
                // The size in this dimension isn't constant. Treating it
                // as zero effectively prohibits supplying initial values,
                // but does still allow the array creation (with the correct
                // size at run time).
                LengthInThisDimension = 0;
                ThisDimensionIsNotConstant = true;
            }
        }
        else
        {
            if (DimensionInitializer)
            {
                // Check to see if all dimensions of the array are initialized. If there
                // is an initializer, it must be an aggregate initializer. If there is
                // no initializer (represented as a List with a Null left operand),
                // this dimension of the array will be empty, i.e.
                // initialized to zero size, as will all subsequent dimensions.

                if (DimensionInitializer->bilop != SX_LIST)
                {
                    ReportSemanticError(
                        ERRID_ArrayInitializerTooFewDimensions,
                        DimensionInitializer->Loc);

                    return AllocateBadExpression(TextSpan);
                }

                LengthInThisDimension =
                    DimensionInitializer->AsExpressionWithChildren().Left == NULL ?
                        0 :
                        ExpressionListLength(DimensionInitializer);
            }

            if (*DimensionTarget == NULL)
            {
                *DimensionTarget =
                    AllocateExpression(
                        SX_LIST,
                        TypeHelpers::GetVoidType(),
                        NULL,
                        NULL,
                        TextSpan);
            }

            (*DimensionTarget)->AsExpressionWithChildren().Left =
                ProduceConstantExpression(
                    LengthInThisDimension,
                    TextSpan,
                    GetFXSymbolProvider()->GetIntegerType()
                    IDE_ARG(0));
        }

        if (DimensionInitializer)
        {
            DimensionInitializer =
                DimensionInitializer->bilop == SX_LIST ?
                    DimensionInitializer->AsExpressionWithChildren().Left :
                    NULL;
        }

        DimensionCounts[DimensionIndex].DimensionCount = LengthInThisDimension;
        DimensionCounts[DimensionIndex].isNotConstant = ThisDimensionIsNotConstant;
        DimensionTarget = &(*DimensionTarget)->AsExpressionWithChildren().Right;
    }

    if (DimensionInitializer && DimensionInitializer->bilop == SX_LIST)
    {
        ReportSemanticError(
            ERRID_ArrayInitializerTooManyDimensions,
            DimensionInitializer->Loc);

        return AllocateBadExpression(TextSpan);
    }

    if (m_IsGeneratingXML)
    {
        Type *ElementType = TypeHelpers::GetElementType(ResultType);

        if (TypeHelpers::IsBadType(ElementType))
        {
            return AllocateBadExpression(TextSpan);
        }

        ConvertAllArrayElements(Initializer, ElementType);

        return
            AllocateExpression(
                SX_CREATE_ARRAY,
                ResultType,
                DimensionSizes,
                Initializer,
                TextSpan);
    }

    // Deal with arrays passed as attribute arguments
    //
    if (IsAppliedAttributeContext())
    {
        ILTree::Expression *ArrayInitializer = NULL;

        if (Initializer && (Initializer->AsExpressionWithChildren().Left || Initializer->AsExpressionWithChildren().Right))
        {
            ArrayInitializer =
                InitializeArrayElementsAsBlob(
                    ResultType,
                    Initializer,
                    0,
                    DimensionCounts);
        }

        // Arrays used as attribute arguments require explcitly specifying the
        // values for all the elements. Note that we check this only for 1-D
        // arrays because only 1-D arrays are allowed as attribute arguments.
        // For all other array arguments, a more general error is given later
        // on.
        //
        if (ArrayInitializer == NULL &&
            DimensionCount == 1 &&
            DimensionCounts[0].DimensionCount != 0)
        {
            // Arrays used as attribute arguments are required to explicitly
            // specify values for all the elements.

            ReportSemanticError(
                ERRID_MissingValuesForArraysInApplAttrs,
                Initializer ? Initializer->AsExpressionWithChildren().Loc : TextSpan);

            return AllocateBadExpression(TextSpan);
        }

        return
            AllocateExpression(
                SX_CREATE_ARRAY,
                ResultType,
                DimensionSizes,
                ArrayInitializer,
                TextSpan);
    }


    // If there's no initialization to be done, skip the temporary
    if (Initializer == NULL ||
        (Initializer->AsExpressionWithChildren().Left == NULL && Initializer->AsExpressionWithChildren().Right == NULL))
    {
        return
            AllocateExpression(
                SX_NEW_ARRAY,
                ResultType,
                DimensionSizes,
                TextSpan);
    }

    ArrayTemporary = AllocateShortLivedTemporary(ResultType);

    // Indices is an array used to hold the indexing information
    // necessary to index to a particular initialized element.
    unsigned IndicesScratch[5];
    unsigned *Indices = IndicesScratch;
    if (DimensionIndex > 5)
    {
        Indices = new(m_TreeStorage) unsigned[DimensionIndex];
    }

    ILTree::Expression *ElementInitializations = NULL;

    // If the initializer is omitted, or if it is the canonical form
    // for an empty aggregate initializer (no element, no next),
    // skip the element initializations.

    if (Initializer && (Initializer->AsExpressionWithChildren().Left || Initializer->AsExpressionWithChildren().Right))
    {
        ElementInitializations =
            InitializeArrayElements(
                ArrayTemporary,
                ResultType,
                Initializer,
                0,
                Indices,
                DimensionIndex,
                DimensionCounts,
                StorageIndices);
    }

    ILTree::Expression *ArrayCreation =
        AllocateExpression(
            SX_ASG,
            TypeHelpers::GetVoidType(),
            ReferToSymbol(TextSpan, ArrayTemporary, chType_NONE, NULL, NULL, ExprNoFlags),
            AllocateExpression(SX_NEW_ARRAY, ResultType, DimensionSizes, TextSpan),
            TextSpan);

    if (ElementInitializations)
    {
        if (IsBad(ElementInitializations))
        {
            return AllocateBadExpression(TextSpan);
        }

        // Prepend the creation of the array to the element initializations.

        ArrayCreation =
            AllocateExpression(
                SX_SEQ,
                TypeHelpers::GetVoidType(),
                ArrayCreation,
                ElementInitializations,
                TextSpan);
    }

    // Make the result of the expression an RValue reference
    // to the array temporary.

    return
        AllocateExpression(
            SX_SEQ_OP2,
            ResultType,
            ArrayCreation,
            AllocateSymbolReference(
                ArrayTemporary,
                ResultType,
                NULL,
                TextSpan),
            TextSpan);
}

void
Semantics::ConvertAllArrayElements
(
    ExpressionList *Initializer,
    Type *ElementType
)
{
    for (ExpressionList *ElementInit = Initializer;
         ElementInit;
         ElementInit = ElementInit->AsExpressionWithChildren().Right)
    {
        VSASSERT(ElementInit->bilop == SX_LIST, "An array initializer is not a list tree.");

        ILTree::Expression *Element = ElementInit->AsExpressionWithChildren().Left;

        if (Element)
        {
            if (Element->bilop == SX_LIST)
            {
                ConvertAllArrayElements(Element, ElementType);
            }
            else
            {
                ElementInit->AsExpressionWithChildren().Left =
                    ConvertWithErrorChecking(
                        Element,
                        ElementType,
                        ExprForceRValue);
            }
        }
    }
}

ILTree::Expression *
Semantics::InitializeArrayElements
(
    Variable *ArrayTemporary,
    ArrayType *InitializedArrayType,
    ExpressionList *Initializer,
    unsigned Dimension,
    _In_count_(IndicesCount) unsigned Indices[],
    unsigned IndicesCount,
    DIMCOUNTS DimensionCounts[]
)
{
    return
        InitializeArrayElements(
            ArrayTemporary,
            InitializedArrayType,
            Initializer,
            Dimension,
            Indices,
            IndicesCount,
            DimensionCounts,
            NULL);
}

ILTree::Expression *
Semantics::InitializeArrayElements
(
    Variable *ArrayTemporary,
    ArrayType *InitializedArrayType,
    ExpressionList *Initializer,
    unsigned Dimension,
    _In_count_(IndicesCount) unsigned Indices[],
    unsigned IndicesCount,
    DIMCOUNTS DimensionCounts[],
    unsigned StorageIndices[]
)
{
    unsigned DimensionCount = InitializedArrayType->GetRank();
    unsigned InitializerCount = 0;

    if (Dimension >= IndicesCount ||
        !ValidateArrayInitializer(
            Initializer,
            Dimension,
            DimensionCount,
            DimensionCounts,
            InitializerCount))
    {
        return MakeBad(Initializer);
    }

    if (InitializerCount == 0 || DimensionCount != (USHORT)DimensionCount) // There will be a narrowing cast farther down
    {
        return NULL;
    }

    Type *ElementType = TypeHelpers::GetElementType(InitializedArrayType);

    if (TypeHelpers::IsBadType(ElementType))
    {
        // There's nothing to mark Bad.
        return NULL;
    }

    bool InitializationIsBad = false;

    // If supplied, the StorageIndices array contains the order in which the array initalizer should fill the
    // array.  If not supplied, the array initalizer will fill sequentially from the front.

    unsigned CurrentElement = 0;
    unsigned IndexInThisDimension = StorageIndices ? StorageIndices[CurrentElement] : CurrentElement;

    ILTree::Expression *Result = NULL;
    ILTree::Expression **PreviousElement = NULL;

    for (ExpressionList *List = Initializer; List; List = List->AsExpressionWithChildren().Right)
    {
        ILTree::Expression *ElementResult = NULL;

        Indices[Dimension] = IndexInThisDimension;

        if (Dimension == DimensionCount - 1)
        {
            if (List->AsExpressionWithChildren().Left->bilop == SX_LIST)
            {
                ReportSemanticError(
                    ERRID_ArrayInitializerTooManyDimensions,
                    List->AsExpressionWithChildren().Left->Loc);

                return MakeBad(Initializer);
            }
            ILTree::Expression *ElementValue =
                ConvertWithErrorChecking(
                    List->AsExpressionWithChildren().Left,
                    ElementType,
                    ExprForceRValue);

            // Create an indexing into the target cell in the array.

            ExpressionList *IndexList = NULL;
            ExpressionList **ListTarget = &IndexList;
            for (unsigned Index = 0; Index < DimensionCount; Index++)
            {
                ILTree::Expression *IndexValue =
                    ProduceConstantExpression(
                        Indices[Index],
                        ElementValue->Loc,
                        GetFXSymbolProvider()->GetIntegerType()
                        IDE_ARG(0));

                ExpressionList *ListElement =
                    AllocateExpression(
                        SX_LIST,
                        TypeHelpers::GetVoidType(),
                        IndexValue,
                        NULL,
                        IndexValue->Loc);

                *ListTarget = ListElement;
                ListTarget = &ListElement->AsExpressionWithChildren().Right;
            }

            ILTree::Expression *ArrayIndex =
                AllocateExpression(
                    SX_INDEX,
                    ElementType,
                    ReferToSymbol(
                        ElementValue->Loc,
                        ArrayTemporary,
                        chType_NONE,
                        NULL,
                        NULL,
                        ExprNoFlags),
                    IndexList,
                    IndexList->Loc);
#pragma warning(disable:22006) // I round-tripped this cast above.
            ArrayIndex->AsIndexExpression().DimensionCount = (USHORT) DimensionCount;
#pragma warning(default:22006) // I round-tripped this cast above.

            // Synthesize an assignment to the array element.

            ElementResult =
                AllocateExpression(
                    SX_ASG,
                    TypeHelpers::GetVoidType(),
                    ArrayIndex,
                    ElementValue,
                    ElementValue->Loc);
        }
        else
        {
            ElementResult =
                InitializeArrayElements(
                    ArrayTemporary,
                    InitializedArrayType,
                    List->AsExpressionWithChildren().Left,
                    Dimension + 1,
                    Indices,
                    IndicesCount,
                    DimensionCounts);
        }

        CurrentElement++;
        IndexInThisDimension = StorageIndices ? StorageIndices[CurrentElement] : CurrentElement;

        if (ElementResult == NULL)
        {
            // An empty initialization doesn't need to be added to the result trees.
            continue;
        }

        if (IsBad(ElementResult))
        {
            InitializationIsBad = true;
        }

        // PreviousElement is the address of the tree for the previously
        // processed element, which may itself be a sequence.

        if (PreviousElement)
        {
            // If the previous element is a sequence, find the end of the sequence.
            while ((*PreviousElement)->bilop == SX_SEQ)
            {
                PreviousElement = &(*PreviousElement)->AsExpressionWithChildren().Right;
            }

            ILTree::Expression *Sequence =
                AllocateExpression(
                    SX_SEQ,
                    TypeHelpers::GetVoidType(),
                    *PreviousElement,
                    ElementResult,
                    ElementResult->Loc);

            *PreviousElement = Sequence;
            PreviousElement = &Sequence->AsExpressionWithChildren().Right;
        }
        else
        {
            Result = ElementResult;
            PreviousElement = &Result;
        }
    }

    if (InitializationIsBad)
    {
        MakeBad(Result);
    }

    return Result;
}

ILTree::Expression *
Semantics::InitializeArrayElementsAsBlob
(
    ArrayType *InitializedArrayType,
    ExpressionList *Initializer,
    unsigned Dimension,
    DIMCOUNTS DimensionCounts[]
)
{
    unsigned DimensionCount = InitializedArrayType->GetRank();
    unsigned InitializerCount = 0;

    if (!ValidateArrayInitializer(
            Initializer,
            Dimension,
            DimensionCount,
            DimensionCounts,
            InitializerCount))
    {
        return MakeBad(Initializer);
    }

    if (InitializerCount == 0)
    {
        return NULL;
    }

    Type *ElementType = TypeHelpers::GetElementType(InitializedArrayType);

    if (TypeHelpers::IsBadType(ElementType))
    {
        return MakeBad(Initializer);
    }

    bool InitializationIsBad = false;

    for (ExpressionList *List = Initializer; List; List = List->AsExpressionWithChildren().Right)
    {
        ILTree::Expression *ElementResult = NULL;

        if (Dimension == DimensionCount - 1)
        {
            if (List->AsExpressionWithChildren().Left->bilop == SX_LIST)
            {
                ReportSemanticError(
                    ERRID_ArrayInitializerTooManyDimensions,
                    List->AsExpressionWithChildren().Left->Loc);

                return MakeBad(Initializer);
            }

            ElementResult =
                ConvertWithErrorChecking(
                    List->AsExpressionWithChildren().Left,
                    ElementType,
                    ExprForceRValue);

            List->AsExpressionWithChildren().Left = ElementResult;
        }
        else
        {
            ElementResult =
                InitializeArrayElementsAsBlob(
                    InitializedArrayType,
                    List->AsExpressionWithChildren().Left,
                    Dimension + 1,
                    DimensionCounts);
        }

        if (ElementResult == NULL)
        {
            // An empty initialization doesn't need to be added to the result trees.
            continue;
        }

        if (IsBad(ElementResult))
        {
            InitializationIsBad = true;
        }
    }

    if (InitializationIsBad)
    {
        MakeBad(Initializer);
    }

    return Initializer;
}

bool
Semantics::ValidateArrayInitializer
(
    ExpressionList *Initializer,
    unsigned Dimension,
    unsigned DimensionCount,
    DIMCOUNTS DimensionCounts[],
    unsigned &InitializerCount
)
{
    if (Initializer->bilop != SX_LIST ||
        Dimension + 1 < 1 || // Overflow
        (Initializer->AsExpressionWithChildren().Left == NULL &&
         Dimension + 1 < DimensionCount))
    {
        ReportSemanticError(
            ERRID_ArrayInitializerTooFewDimensions,
            Initializer->Loc);

        return false;
    }

    InitializerCount =
        Initializer->AsExpressionWithChildren().Left == NULL ?
            0 :
            ExpressionListLength(Initializer);
#pragma warning(suppress:22008) // Dimension will always be bounded within the span of DimensionCounts in our calls
    unsigned ElementCount = DimensionCounts[Dimension].DimensionCount;

    if (InitializerCount != ElementCount)
    {
        if (ElementCount == 0 && DimensionCounts[Dimension].isNotConstant)
        {
            ReportSemanticError(
                ERRID_ArrayInitializerForNonConstDim,
                Initializer->Loc);
        }
        else
        {
            ValidateElementCount(ElementCount, InitializerCount, Initializer->Loc);
        }

        return false;
    }

    return true;
}

ILTree::Expression *
Semantics::InterpretInitializer
(
    ParseTree::Initializer *Init,
    Type *TargetType
)
{
    return
        InterpretInitializer(
            Init,
            TargetType,
            ExprForceRValue);
    }

ILTree::Expression *
Semantics::InterpretInitializer
(
    ParseTree::Initializer *Init,
    Type *TargetType,
    ExpressionFlags Flags
)
{
    ParseTree::Expression *InitialValue = Init->AsExpression()->Value;
    AssertIfNull(InitialValue);

    // 


    if (ParseTree::Expression::Deferred == InitialValue->Opcode)
    {
        InitialValue = InitialValue->AsDeferred()->Value;
    }

    /*if (ParseTree::Expression::ArrayInitializer == InitialValue->Opcode)
    {
        ExpressionList *Initializer =
            InterpretArrayInitializerList(
                InitialValue->AsArrayInitializer()->Elements,
                ExprForceRValue);

        if (Initializer && IsBad(Initializer))
        {
            return AllocateBadExpression(InitialValue->TextSpan);
        }

        if (NULL == TargetType)
        {
            // A caller that cares about an error message in this case is expected to catch
            // initializing a non-array with an aggregate initializer.
            // ReportSemanticError(ERRID_AggrInitNYI, InitialValue->TextSpan);
            return AllocateBadExpression(InitialValue->TextSpan);
        }

        if (!TypeHelpers::IsArrayType(TargetType))
        {
            // A caller that cares about an error message in this case is expected to catch
            // initializing a non-array with an aggregate initializer.

            // VSASSERT(m_IsGeneratingXML, "Aggregate initializer for non-array snuck through.");

            // ReportSemanticError(ERRID_AggrInitNYI, InitialValue->TextSpan);
            return AllocateBadExpression(InitialValue->TextSpan);
        }

        return
            InitializeArray(
                Initializer,
                TargetType->PArrayType(),
                NULL,
                InitialValue->TextSpan);
    }*/

    ExpressionFlags valueFlags = TargetType ? Flags & ~ExprForceRValue : Flags;

    ILTree::Expression * pRet = 
        InterpretExpressionWithTargetType(
            InitialValue,
            valueFlags,
            TargetType);

    if ((Flags & ExprForceRValue) && TargetType && ! IsBad(pRet))
    {
        pRet = MakeRValue(pRet);
    }

    return pRet;
}

ILTree::Expression *
Semantics::InterpretUnaryOperation
(
    ParseTree::Expression::Opcodes Opcode,
    const Location &ExpressionLocation,
    ILTree::Expression *Operand,
    ExpressionFlags Flags
)
{
    ILTree::Expression *Result = NULL;

    if (IsNothingLiteral(Operand))
    {
        Operand = Convert(Operand, GetFXSymbolProvider()->GetIntegerType(), ExprNoFlags, ConversionWidening);
    }

    BILOP BoundOpcode = MapOperator(Opcode);
    bool ResolutionFailed = false;
    Procedure *OperatorMethod = NULL;
    GenericBinding *OperatorMethodGenericContext = NULL;
    bool LiftedNullable = false;

    Type *ResultType =
        ResolveUnaryOperatorResultType(
            BoundOpcode,
            ExpressionLocation,
            Operand,
            ResolutionFailed,
            OperatorMethod,
            OperatorMethodGenericContext,
            LiftedNullable);

    if (ResolutionFailed)
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    ExpressionFlags ConversionFlags = Flags & ExprMustBeConstant;

    VSASSERT(OperatorMethodGenericContext == NULL || OperatorMethod, "Generic context for null method unexpected!!!");

    if (OperatorMethod)
    {
        // The unary operation resolves to a user-defined operator.
        AssertIfFalse(TypeHelpers::EquivalentTypes(ResultType, TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext)));

        Result =
            InterpretUserDefinedOperator(
                BoundOpcode,
                OperatorMethod,
                OperatorMethodGenericContext,
                ExpressionLocation,
                Operand,
                ConversionFlags);

        if (! IsBad(Result) && OperatorMethod->IsLiftedOperatorMethod())
        {

            // If OperatorMethodGenericContext is defined then it must be GenericTypeBinding
            ThrowIfFalse(OperatorMethodGenericContext == NULL || OperatorMethodGenericContext->IsGenericTypeBinding());

            GenericTypeBinding* pGenericTypeBinding = OperatorMethodGenericContext != NULL ? 
                OperatorMethodGenericContext->PGenericTypeBinding() : 
                NULL;

            if
            (
                !
                (
                    IsValidInLiftedSignature(OperatorMethod->GetType(), pGenericTypeBinding) &&
                    IsValidInLiftedSignature(OperatorMethod->GetFirstParam()->GetType(), pGenericTypeBinding)
                )
            )
            {
                MakeBad(Result);
                ReportSemanticError
                (
                    ERRID_UnaryOperand2,
                    Result->Loc,
                    m_Compiler->OperatorToString(OperatorMethod->PLiftedOperatorMethod()->GetActualProc()->GetAssociatedOperatorDef()->GetOperator()),
                    Operand->ResultType
                );
            }
        }

        return Result;
    }

    // Option Strict disallows all unary operations on Object operands. Otherwise just warn.
    if (TypeHelpers::IsRootObjectType(Operand->ResultType))
    {
        AssertIfTrue(LiftedNullable);
        if (m_UsingOptionTypeStrict)
        {
            ReportSemanticError(
                ERRID_StrictDisallowsObjectOperand1,
                Operand->Loc,
                Opcode);

            return AllocateBadExpression(ExpressionLocation);
        }
        else if (WarnOptionStrict())
        {
            ReportSemanticError(
                WRNID_ObjectMath2,
                Operand->Loc,
                Opcode);
        }
    }

    Operand = ConvertWithErrorChecking(Operand, ResultType, ConversionFlags);

    if (IsBad(Operand))
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    if (Opcode == ParseTree::Expression::Not && TypeHelpers::IsBooleanType(ResultType))
    {
        AssertIfTrue(LiftedNullable);

        Result = NegateBooleanExpression(Operand);
        Result->Loc = ExpressionLocation;

        return Result;
    }

    if (IsConstant(Operand))
    {
        AssertIfTrue(LiftedNullable);

        if (AllowsCompileTimeOperations(ResultType) &&
            AllowsCompileTimeOperations(Operand->ResultType))
        {
            Result =
                PerformCompileTimeUnaryOperation(
                    BoundOpcode,
                    ResultType,
                    ExpressionLocation,
                    Operand);
        }
        else if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                ExpressionLocation);

            return AllocateBadExpression(ExpressionLocation);
        }
    }
    else
    {
        AssertIfTrue(HasFlag(Flags, ExprMustBeConstant));
    }

    if (Result == NULL)
    {
        Result =
            AllocateExpression(
                BoundOpcode,
                ResultType,
                Operand,
                ExpressionLocation);

        if (LiftedNullable)
        {
            Result->uFlags |= SXF_OP_LIFTED_NULLABLE;
        }
    }
    return Result;
}

bool ValidateAwaitPattern(_In_ ILTree::Expression *expr, _In_ STRING *expectedMethodName, _In_ BCSYM *expectedThisType, _In_opt_ BCSYM *expectedArgumentType, bool expectedLateCall, bool allowExtensionMethod, bool isPropertyGet)
{
    if (expr == NULL || expectedMethodName == NULL || expectedThisType == NULL || (allowExtensionMethod && isPropertyGet))
    {
        VSFAIL("bad arguments");
        return false;
    }

    // Assuming a parse-tree of the form "b.f([arg1])" which has been interpreted,
    // this function validates whether it generated a call expression to a simple non-conditional instance/extension
    // method named "f" that took exactly the argument that was passed (if any)
    //
    // Seems obvious, right? Well, it's not...
    // VB lets b.f(arg) take optional extra parameters, lets it be a property-getter, and so on.
    // These are the weird things that this function is designed to filter out.
    //
    // This function is not really general-purpose because of some limitations:
    // (0) It only works with 0 or 1 arguments
    // (1) It doesn't do much validation on the shape of late-bound calls. That's because, for Await purposes,
    //     their shape is predictable enough that it's not worth validating
    // (2) The "expectedMethodName" trick won't work if your expectedMethodName was "Invoke" !!!
    //     e.g. if you did a.Invoke() but Invoke was a delegate field then it'd wind up as a.Invoke.Invoke()
    //     and we wouldn't notice.
    // (3) It doesn't distinguish between b.SharedMethod(b,optional c) and b.ExtensionMethod(c)
    // (4) And of course this function assumes the expr came from a parse-tree of the form "b.f(arg1, ...)"
    // (5) We have no way to distinguish whether a.f() bound to [disallowed] a shared method TA.f(default)
    //     or to [allowed] an extension method EXTENSIONMODULE.f(a). Resolution: "Won't Fix" (Dev11#161008)
    // 

    if (expr->bilop == SX_LATE)
    {
        if (!expectedLateCall)
        {
            return false;
        }

        // NOTE: We don't bother doing validation of "suppliedArgumentCount" here.
        // It's just too fiddly, and won't catch any bugs when used by the Await pattern.
        return true;
    }

    if (expr->bilop == SX_CALL)
    {
        if (expectedLateCall)
        {
            return false;
        }

        ILTree::CallExpression* pCall = &expr->AsCallExpression();

        ILTree::Expression *actualThis = pCall->MeArgument;
        ILTree::Expression *actualArgument = NULL;

        if (pCall->MeArgument != NULL)
        {
            // was an instance call
            actualArgument = pCall->Right;
        }
        else
        {
            // was either an extension call, or a shared method being called off an instance
            if (!allowExtensionMethod)
            {
                // could be either an extension method or a shared method but shared methods
                // are always disallowed and if we're not allowing extension methods then
                // neither case is valid.
                return false;
            }

            if (pCall->Right == NULL)
            {
                actualThis = NULL;
            }
            else if (pCall->Right->bilop == SX_LIST)
            {
                actualThis = pCall->Right->AsExpressionWithChildren().Left;
                actualArgument = pCall->Right->AsExpressionWithChildren().Right;
            }
            else
            {
                actualThis = pCall->Right;
            }
        }
        if (actualArgument != NULL && actualArgument->bilop == SX_LIST)
        {
            ILTree::Expression *remainingArguments = actualArgument->AsExpressionWithChildren().Right;
            if (remainingArguments != NULL)
            {
                return false; // we'll only ever entertain 0 or 1 arguments
            }

            actualArgument = actualArgument->AsExpressionWithChildren().Left;
        }

        if (actualThis == NULL)
        {
            return false; // detects if a.f() invokes a shared method off an instance
        }

        // Dev11#161008: You'd have thought we could test that actualThis->ResultType was the same as expectedThisType
        // (or at least, the result of chasing through pointers). But that's not always true, e.g. in the case
        // of "Await x" where x is a generic type parameter T with a Task constraint: expectedThisType will be T,
        // and actualThis->ResultType will be that Task constraint.

        if (actualArgument == NULL)
        {
            if (expectedArgumentType != NULL)
            {
                return false; // shouldn't happen
            }
        }
        else
        {
            if (expectedArgumentType == NULL)
            {
                return false; // detects a.f(optional x)
            }
            if (!TypeHelpers::EquivalentTypes(actualArgument->ResultType->ChaseThroughPointerTypes(), expectedArgumentType))
            {
                return false; // might happen in odd circumstances confusing shared methods with extension methods & optionals
            }
        }

        if (pCall->Left == NULL || pCall->Left->bilop != SX_SYM)
        {
            return false; // I can't imagine why this would arise
        }
        auto pSymbol = pCall->Left->AsSymbolReferenceExpression().Symbol;
        if (!pSymbol->IsProc())
        {
            return false; // I can't imagine why this would arise
        }
        auto pProc = pSymbol->PProc();

        if (!isPropertyGet && pProc->IsPropertyGet())
        {
            return false; // detects cases like a.f() being interpreted as a get of property "f"
        }
        else if (isPropertyGet && !pProc->IsPropertyGet())
        {
            return false; // detects cases like a.f being interpreted as a call to sub "f"
        }

        if ((isPropertyGet && CompareNoCaseN(pProc->GetAssociatedPropertyDef()->GetName(), expectedMethodName, wcslen(expectedMethodName)) != 0) ||
            (!isPropertyGet && CompareNoCaseN(pProc->GetName(), expectedMethodName, wcslen(expectedMethodName)) != 0))
        {
            return false; // detects cases like a.f() being interpreted as a.f.Invoke()
        }

        if (!isPropertyGet)
        {
            ConditionalString *pCondition = NULL;
            pProc->GetPWellKnownAttrVals()->GetConditionalData(&pCondition);
            if (pCondition != NULL)
            {
                return false; // detects cases like <Conditional("X")> Sub GetResult()
            }
        }

        return true;
    }

    return false;

}


ILTree::Expression *
Semantics::InterpretAwaitExpression
(
    const Location &loc,
    ParseTree::Expression *OperandTree,
    ExpressionFlags Flags
)
{
    if (m_InQuery)
    {
        ReportSemanticError(ERRID_BadAsyncInQuery, loc);
        return AllocateBadExpression(loc);
    }

    if (HasFlag(Flags, ExprMustBeConstant))
    {
        ReportSemanticError(ERRID_RequiredConstExpr, loc);
        return AllocateBadExpression(loc);
    }

    if (!HasFlag(Flags, ExprSpeculativeBind))
    {
        SetLocalSeenAwait();
    }

    ILTree::ResumableKind resumableKind = GetLocalResumableKind();

    if (resumableKind != ILTree::UnknownResumable &&
        resumableKind != ILTree::TaskResumable &&
        resumableKind != ILTree::SubResumable)
    {
        // Note: we allow binding to succeed in the case where GetCurrentResumableInfo() returns NULL or has unknown mode.
        // That will likely come from IDE situations where they ask us to parse+bind a fragment.
        // (it's only later on, at codegen time, that it's legitimate to bail in cases where there's no resumable).
        VSASSERT(resumableKind != ILTree::UnknownResumable, "Oops! someone should have called IndicateResumable before interpreting the body of this lambda");
        if (resumableKind != ILTree::ErrorResumable) ReportBadAwaitInNonAsync(loc);
        return AllocateBadExpression(loc);
    }

    ILTree::Expression* Operand = (OperandTree==NULL) ? NULL : InterpretExpression(OperandTree, ExprNoFlags);

    if (Operand == NULL)
    {
        // This should only happen if the original parse tree was null, in
        // which case the parser should have already reported an error.
        return AllocateBadExpression(loc);
    }
    else if (IsNothingLiteral(Operand))
    {
        ReportSemanticError(ERRID_BadAwaitNothing, loc);
        return AllocateBadExpression(loc);
    }
    else if (IsBad(Operand))
    {
        // Have already reported an error. Not much else we can do.
        return AllocateBadExpression(loc);
    }

    // If the user tries to do "await f()" or "await expr.f()" where f is an async sub,
    // then we'll give a more helpful error message...
    if (Operand->ResultType->IsVoidType() && Operand->bilop == SX_CALL)
    {
        ILTree::CallExpression *pCall = &Operand->AsCallExpression();
        if (pCall->Left != NULL && pCall->Left->bilop == SX_SYM)
        {
            ILTree::SymbolReferenceExpression *pTarget = &pCall->Left->AsSymbolReferenceExpression();
            if (pTarget->Symbol != NULL && pTarget->Symbol->IsMethodImpl())
            {
                BCSYM_MethodImpl *pMethod = pTarget->Symbol->PMethodImpl();
                if (pMethod->IsAsyncKeywordUsed())
                {
                    ReportSemanticError(ERRID_CantAwaitAsyncSub1, pTarget->Loc, pMethod->GetName());
                    return AllocateBadExpression(loc);
                }
            }
        }
    }

    Operand = MakeRValue(Operand);

    if (IsBad(Operand))
    {
        // have already reported an error. Not much else we can do.
        return AllocateBadExpression(loc);
    }
    else if (Operand->ResultType->IsObject())
    {
        if (m_UsingOptionTypeStrict)
        {
            ReportSemanticError(ERRID_StrictDisallowsLateBinding, loc);
            return AllocateBadExpression(loc);
        }
        else if (WarnOptionStrict())
        {
            ReportSemanticError(WRNID_LateBindingResolution, loc);
        }
    }
    else if (!GetFXSymbolProvider()->IsTypeAvailable(FX::INotifyCompletionType) ||
             !GetFXSymbolProvider()->IsTypeAvailable(FX::ICriticalNotifyCompletionType))
    {
        // An error has already been reported by IndicateLocalResumable
        return AllocateBadExpression(loc);
    }

    if (HasFlag(Flags, ExprIsAssignmentTarget))
    {
        ReportSemanticError(ERRID_LValueRequired, loc);
        return AllocateBadExpression(loc);
        // Normally this test is done in ApplyContextSpecificSemantics right at the end after InterpretExpression
        // But what with Operand being an intermediate evaluation, we have to do this check up front.
        // (Also all the rest of the functionality that goes along with ApplyContextSpecificSemantics -- like
        // default property transformation -- is not appropriate for the operand of await).
    }

    bool useLateBoundPattern = (Operand->ResultType->IsObject());



    ParserHelper ph(&m_TreeStorage, loc);
    ILTree::Expression *expr = NULL;
    ILTree::Expression *GetAwaiterDummy = NULL;
    ILTree::Expression *IsCompletedDummy = NULL;
    ILTree::Expression *GetResultDummy = NULL;

    ParseTree::Expression *pOperandDummy = ph.CreateConversion(ph.CreateNothingConst(), ph.CreateBoundType(Operand->ResultType, loc));

    BackupValue<bool> backup_report_errors(&m_ReportErrors);
    m_ReportErrors = false;
    // operandDummy.GetAwaiter()
    STRING *GetAwaiterName = m_Compiler->AddString(L"GetAwaiter");
    ParseTree::Expression *pGetAwaiterMethod = ph.CreateQualifiedExpression(pOperandDummy, ph.CreateNameExpression(GetAwaiterName));
    ParseTree::CallOrIndexExpression *pGetAwaiterCall = ph.CreateMethodCall(pGetAwaiterMethod, NULL, loc);
    expr = InterpretExpression(pGetAwaiterCall, Flags & ~(ExprResultNotNeeded | ExprPropagatePropertyReference) | ExprForceRValue);
    //
    backup_report_errors.Restore();
    //
    if (expr == NULL || IsBad(expr))
    {
        // A simple test for if you tried to await something completely unawaitable, e.g. "await integer"
        // Note that NO error has already been reported, because m_ReportErrors was set to "false".
        ReportSemanticError(ERRID_AwaitPattern1, loc, Operand->ResultType); // "Not awaitable"
        return AllocateBadExpression(loc);
    }

    // All subsequent tests detect that the thing tried to be awaitable, but failed for some reason
    if (!ValidateAwaitPattern(expr, GetAwaiterName, Operand->ResultType, NULL, useLateBoundPattern, true /*allowExtensionMethod*/, false /*isPropertyGet*/))
    {
        // this detects if it was something other than a simple instance/extension/late call

        // Note: it's possible in edge cases that "expr" was bound succesfully -- but with a warning --
        // to something that didn't validate as an await pattern. E.g. t.GetAwaiter() where GetAwaiter is
        // a shared method. Then you'll get the warning above, plus an error message from here.
        // It's a shame to get both messages. But not a big enough shame to justify any new codepaths.
    }
    else if (expr->ResultType==NULL || expr->ResultType->IsBad() || expr->ResultType->IsVoidType() ||
             (expr->ResultType->IsObject() && !useLateBoundPattern))
    {
        // this detects if its return type was invalid
        VSASSERT(!expr->ResultType->IsBad(), "Unexpected: how can a non-bad expr have a bad result-type? and did it already report an error, or not?");
    }
    else
    {
        GetAwaiterDummy = expr;
    }

    if (GetAwaiterDummy == NULL)
    {
        ReportSemanticError(ERRID_BadGetAwaiterMethod1, loc, Operand->ResultType); 
        return AllocateBadExpression(loc);
    }



    ParseTree::Expression *pAwaiterDummy = ph.CreateConversion(ph.CreateNothingConst(), ph.CreateBoundType(GetAwaiterDummy->ResultType, loc));
    
    if (!GetFXSymbolProvider()->IsTypeAvailable(FX::INotifyCompletionType) ||
        !GetFXSymbolProvider()->IsTypeAvailable(FX::ICriticalNotifyCompletionType))
    {
        ReportMissingType(FX::ActionType, loc);
        return AllocateBadExpression(loc);
    }

    // awaiterdummy.IsCompleted
    // nb. the awaiterdummy will eventually come from fields in the StateMachine class
    STRING *IsCompletedName = m_Compiler->AddString(L"IsCompleted");
    ParseTree::Expression *pIsCompletedAccess = ph.CreateQualifiedExpression(pAwaiterDummy, ph.CreateNameExpression(IsCompletedName));

    {
        // If the access to IsCompleted is late-bound, turn off error reporting
        // while interpreting the access to IsCompleted. We will have already
        // reported a late-binding warning for the Await expression, and this
        // prevents us from reporting the same warning again.
        BackupValue<bool> backup_ReportErrors(&m_ReportErrors);

        m_ReportErrors &= !useLateBoundPattern;
        expr = InterpretExpression(pIsCompletedAccess, Flags & ~(ExprResultNotNeeded | ExprIsExplicitCallTarget) | ExprForceRValue);

        backup_ReportErrors.Restore();
    }

    //
    if (expr == NULL || IsBad(expr))
    {
        // an error has already been reported

        // (except in the late-bound case which suppresses errors above..)
        if (useLateBoundPattern)
        {
            VSFAIL("If we're binding Await latebound, then how can an error have been reported on IsCompleted?");
            ReportSemanticError(ERRID_BadIsCompletedOnCompletedGetResult2, loc, GetAwaiterDummy->ResultType, Operand->ResultType);
        }
        return AllocateBadExpression(loc);
    }

    // All subsequent tests detect that the thing tried to be awaitable, but failed for some reason
    if (!ValidateAwaitPattern(expr, IsCompletedName, GetAwaiterDummy->ResultType, NULL, useLateBoundPattern, false /*allowExtensionMethod*/, true /*isPropertyGet*/))
    {
        // this detects if it was something other than a simple instance/extension/late call
    }
    else if (expr->ResultType==NULL || expr->ResultType->IsBad() ||
             (!useLateBoundPattern && !TypeHelpers::EquivalentTypes(expr->ResultType, GetFXSymbolProvider()->GetBooleanType())) ||
             (useLateBoundPattern && !expr->ResultType->IsObject()))
    {
        // this detects if its return type was invalid
        VSASSERT(!expr->ResultType->IsBad(), "Unexpected: how can a non-bad expr have a bad result-type? and did it already report an error, or not?");
    }
    else
    {
        IsCompletedDummy = expr;
    }

    if (IsCompletedDummy == NULL)
    {
        ReportSemanticError(ERRID_BadIsCompletedOnCompletedGetResult2, loc, GetAwaiterDummy->ResultType, Operand->ResultType);
        return AllocateBadExpression(loc);
    }


    // INotifyCompletion
    if (!useLateBoundPattern)
    {
        // Early-bound: we just require that TAwaiter implement INotifyCompletion
        Type *INotifyCompletionType = GetFXSymbolProvider()->GetType(FX::INotifyCompletionType);
        if (!IsOrInheritsFromOrImplements(GetAwaiterDummy->ResultType, INotifyCompletionType))
        {
            ReportSemanticError(ERRID_DoesntImplementAwaitInterface2, loc, GetAwaiterDummy->ResultType, INotifyCompletionType);
            return AllocateBadExpression(loc);
        }
    }


    // awaiterdummy.GetResult()
    expr = NULL;
    STRING *GetResultName = m_Compiler->AddString(L"GetResult");
    ParseTree::Expression *pGetResultMethod = ph.CreateQualifiedExpression(pAwaiterDummy, ph.CreateNameExpression(GetResultName));
    ParseTree::CallOrIndexExpression *pGetResultCall = ph.CreateMethodCall(pGetResultMethod, NULL, loc);

    {
        // If the call to GetAwaiter is late-bound, turn off error reporting
        // while interpreting the call to GetResult. We will have already
        // reported a late-binding warning for the Await expression, and this
        // prevents us from reporting the same warning again.
        BackupValue<bool> backup_ReportErrors(&m_ReportErrors);

        m_ReportErrors &= !useLateBoundPattern;
        expr = InterpretExpression(pGetResultCall, Flags & ~(ExprPropagatePropertyReference));

        backup_ReportErrors.Restore();
    }
    
    if (expr == NULL || IsBad(expr))
    {
        // an error has already been reported

        // (except in the late-bound case which suppresses errors above..)
        if (useLateBoundPattern)
        {
            VSFAIL("If we're binding Await latebound, then how can an error have been reported on GetResult?");
            ReportSemanticError(ERRID_BadIsCompletedOnCompletedGetResult2, loc, GetAwaiterDummy->ResultType, Operand->ResultType);
        }
        return AllocateBadExpression(loc);
    }


    // All subsequent tests detect that the thing tried to be awaitable, but failed for some reason
    if (!ValidateAwaitPattern(expr, GetResultName, GetAwaiterDummy->ResultType, NULL, useLateBoundPattern, false /*allowExtensionMethod*/, false /*isPropertyGet*/))
    {
        // this detects if it was something other than a simple instance/extension/late call
    }
    else if (expr->ResultType==NULL || expr->ResultType->IsBad() ||
             (useLateBoundPattern && !expr->ResultType->IsObject()))
    {
        // this detects if its return type was invalid
        VSASSERT(!expr->ResultType->IsBad(), "Unexpected: how can a non-bad expr have a bad result-type? and did it already report an error, or not?");
    }
    else
    {
        GetResultDummy = expr;
    }

    if (GetResultDummy == NULL)
    {
        ReportSemanticError(ERRID_BadIsCompletedOnCompletedGetResult2, loc, GetAwaiterDummy->ResultType, Operand->ResultType);
        return AllocateBadExpression(loc);
    }


    if (GetAwaiterDummy == NULL || IsCompletedDummy  == NULL || GetResultDummy == NULL)
    {
        VSFAIL("We should have ensured that await dummies don't get to here");
        ReportSemanticError(ERRID_BadIsCompletedOnCompletedGetResult2, loc, GetAwaiterDummy->ResultType, Operand->ResultType);
        return AllocateBadExpression(loc);
    }

    ILTree::AwaitExpression *Result = &AllocateExpression(SX_AWAIT, GetResultDummy->ResultType, Operand, loc)->AsAwaitExpression();     // ASYNCTODO L007: what if ResultType was void? what does NULL mean?
    Result->GetAwaiterDummy = GetAwaiterDummy;
    Result->IsCompletedDummy = IsCompletedDummy;
    Result->GetResultDummy = GetResultDummy;

    if (HasFlag(Flags, ExprResultNotNeeded))
    {
        Result->ResultType = TypeHelpers::GetVoidType();
        // Normal procedure for late-binding is to call SetLateCallInvocationProperties() on a SX_LATE call to indicate
        // whether it was a LateGet or a LateCall, and to set its ResultType appropriately.
        // But here we're just going to set its result-type. Later on, in ResumableRewriter, when we
        // rewrite the SX_AWAIT into its constituents, that's where we'll set the late call invocation properties.
    }

    if (resumableKind != ILTree::TaskResumable && resumableKind != ILTree::SubResumable)
    {
        VSASSERT(!HasFlag(Flags, ExprSpeculativeBind), "We should only bind speculatively when inside a resumable context");
        MarkContainingLambdaOrMethodBodyBad();
        // Note: it's imperative that the type of an expression as as
        // interpreted in isolation (ILTree::UnknownResumable) is the same
        // as the type of that expression when interpreted as part of the
        // entire method body (where we know that it's
        // ILTree::SubResumable). That's because the IDE's symbol lookup
        // uses the latter, and its IntelliSense uses the former, and it
        // blindly casts a symbol lookup result to the type indicated by
        // IntelliSense. This will crash if the two are different.
    }

    BILOP opCode = m_StatementLambdaInterpreter ? SB_STATEMENT_LAMBDA : SB_PROC;
    bool InTry=false, InCatch=false, InFinally=false, InSynclock=false;
    ILTree::Statement *EnclosingStatement = NearestEnclosing(m_BlockContext, opCode,  false, InTry, InCatch, InFinally, &InSynclock);
    if (InTry) SetFlag32(Result, SXF_EXITS_TRY);
    if (InCatch || InFinally || InSynclock) ReportSemanticError(ERRID_BadAwaitInTryHandler, loc);
    //
    // Note: elsewhere, in Semantics::InterpretVariableDeclarationStatement, is where we detect+report
    // about an Await used in a static local variable initializer. It doesn't use the NearestEnclosing mechanism.

    return Result;
}

void Semantics::SetLocalSeenAwait()
{
    if (m_ExpressionLambdaInterpreter)
    {
        m_ExpressionLambdaInterpreter->m_SeenAwait = true;
    }
    else if (m_StatementLambdaInterpreter)
    {
        m_StatementLambdaInterpreter->m_SeenAwait = true;
    }
    else if (m_Procedure != NULL)
    {
        bool b1,b2,b3;
        ILTree::Statement *Proc = NearestEnclosing(m_BlockContext, SB_PROC, false, b1, b2, b3);
        if (Proc != NULL) Proc->AsProcedureBlock().fSeenAwait = true;
    }
}

Type* Semantics::GetLocalReturnType()
{
    if (m_ExpressionLambdaInterpreter)
    {
        VSFAIL("Unexpected: there's no part of semantics that should call GetLocalReturnType while we're in an expression lambda. This code-path hasn't even been implemented.");
        return TypeHelpers::GetVoidType();
    }
    else if (m_StatementLambdaInterpreter)
    {
        if (m_StatementLambdaInterpreter->m_Tree) return m_StatementLambdaInterpreter->m_Tree->pReturnType;
        VSFAIL("Unexpected: if m_StatementLambdaInterpreter!=NULL, then we should have its m_Tree that was set in LambdaBodyInterpretStatement::DoInterpretBody");
        return TypeHelpers::GetVoidType();
    }
    else
    {
        if (m_Procedure != NULL) return GetReturnType(m_Procedure);
        return TypeHelpers::GetVoidType();
    }
}

Type* Semantics::GetTypeForLocalReturnStatements()
{
    Type *underlyingType = GetLocalReturnType();

    ILTree::ResumableKind resumableKind = GetLocalResumableKind();
    switch (resumableKind)
    {
        case ILTree::NotResumable:
            return underlyingType;

        case ILTree::SubResumable:
            return TypeHelpers::GetVoidType();

        case ILTree::IteratorResumable:
        case ILTree::IterableResumable:
            // These are NOT allowed to return a value.
            // But we expect the caller to already have reported an error for attempting to use a Return statement,
            // so we won't contribute to any further errors:
            return GetFXSymbolProvider()->GetObjectType();

        case ILTree::TaskResumable:
            {
                if (underlyingType == NULL) return underlyingType;

                if (GetFXSymbolProvider()->IsTypeAvailable(FX::TaskType) && underlyingType == GetFXSymbolProvider()->GetType(FX::TaskType))
                {
                    return TypeHelpers::GetVoidType();
                }

                if (GetFXSymbolProvider()->IsTypeAvailable(FX::GenericTaskType) &&
                    underlyingType->IsGenericBinding() && underlyingType->PGenericBinding()->GetGeneric() == GetFXSymbolProvider()->GetType(FX::GenericTaskType))
                {
                    return underlyingType->PGenericBinding()->GetArgument(0);
                }

                // If the thing was marked as "TaskResumable" even in the absence of a valid return type,
                // an error will already have been reported. So we'll be permissive from now on:
                return GetFXSymbolProvider()->GetObjectType();
            }

        case ILTree::ErrorResumable:
        case ILTree::UnknownResumable:
            // In the first case an error has already been reported, so let's not report more errors
            // In the second case we don't yet know anything, so let's not report unjustifiable errors.
            // By returning "Object" we won't get errors. (apart from people who write "Return AddressOf f"...
            // but if that's the case then we have bigger error fish to fry than to worry about errors here)
            return GetFXSymbolProvider()->GetObjectType();
    }

    VSFAIL("Error: what manner of resumable are we?");
    return GetFXSymbolProvider()->GetObjectType();
}

Type* Semantics::GetTypeForLocalYieldStatements()
{
    Type *underlyingType = GetLocalReturnType();

    ILTree::ResumableKind resumableKind = GetLocalResumableKind();
    switch (resumableKind)
    {
        case ILTree::NotResumable:
        case ILTree::SubResumable:
        case ILTree::TaskResumable:
            // We expect our caller to have already reported an error for putting a "Yield" in one of these.
            // We don't want to contribute to any further messages, so we'll be permissive:
            return GetFXSymbolProvider()->GetObjectType();

        case ILTree::UnknownResumable:
        case ILTree::ErrorResumable:
            // Again, there will have been errors reported as appropriate, so we won't contribute to further errors:
            return GetFXSymbolProvider()->GetObjectType();

        case ILTree::IteratorResumable:
        case ILTree::IterableResumable:
            {
                if (underlyingType == NULL) return underlyingType; // can't do anything useful here

                // In the following code we'll trust that Iterator/Iterable has been correctly determined
                // with respect to the return type. There's no sense in validating it again.
                if ((GetFXSymbolProvider()->IsTypeAvailable(FX::IEnumerableType) && underlyingType == GetFXSymbolProvider()->GetType(FX::IEnumerableType)) ||
                    (GetFXSymbolProvider()->IsTypeAvailable(FX::IEnumeratorType) && underlyingType == GetFXSymbolProvider()->GetType(FX::IEnumeratorType)))
                {
                    return GetFXSymbolProvider()->GetObjectType();
                }

                if ((GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIEnumerableType) &&
                     underlyingType->IsGenericBinding() &&
                     underlyingType->PGenericBinding()->GetGeneric() == GetFXSymbolProvider()->GetType(FX::GenericIEnumerableType)) ||
                    (GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIEnumeratorType) &&
                     underlyingType->IsGenericBinding() &&
                     underlyingType->PGenericBinding()->GetGeneric() == GetFXSymbolProvider()->GetType(FX::GenericIEnumeratorType)))
                {
                    return underlyingType->PGenericBinding()->GetArgument(0);
                }

                // If the thing was marked as "IterResumable" even in the absence of a valid return type,
                // an error will already have been reported. So we'll be permissive from now on:
                return GetFXSymbolProvider()->GetObjectType();
            }

    }

    VSFAIL("Error: what manner of resumable are we?");
    return GetFXSymbolProvider()->GetObjectType();

}


ILTree::ResumableKind Semantics::GetLocalResumableKind()
{
    if (m_ExpressionLambdaInterpreter)
    {
        return m_ExpressionLambdaInterpreter->GetResumableKind();
    }
    else if (m_StatementLambdaInterpreter)
    {
        return m_StatementLambdaInterpreter->GetResumableKind();
    }
    else if (m_ProcedureTree!=NULL)
    {
        return m_ProcedureTree->m_ResumableKind;
    }
    else
    {
        // This is a typical scenario for IDE, where they don't give us any context but still want us to bind
        return ILTree::UnknownResumable;
    }
}

void Semantics::SetLocalResumableInfo(ILTree::ResumableKind newKind, Type* resumableGenericType)
{
    // For IEnumerable<T>, IEnumerator<T>, Task<T>, "genericResumableInfo" is that T.

    if (m_ExpressionLambdaInterpreter)
    {
        VSFAIL("Unexpected: there's no part of semantics that should call SetLocalResumableInfo while we're in an expression lambda. This code-path hasn't even been implemented.");
    }
    else if (m_StatementLambdaInterpreter)
    {
        m_StatementLambdaInterpreter->m_ResumableKind = newKind;
        m_StatementLambdaInterpreter->m_ResumableGenericType = resumableGenericType;
    }
    else if (m_ProcedureTree!=NULL)
    {
        m_ProcedureTree->m_ResumableKind = newKind;
        m_ProcedureTree->m_ResumableGenericType = resumableGenericType;
    }
    else
    {
        // This is a typical scenario for IDE, where they don't give us any context but still want us to bind
    }
}


void Semantics::IndicateLocalResumable()
{
    // CONTRACT:
    // We are entered with "LocalResumableKind == UnknownResumable".
    // We leave with it as one of the other resumable-kinds, or "Unknown" if we couldn't get any context.
    // Moreover, if we leave with it as "Error" or "Unknown", then we will report an error.
    // We may also report errors for other kinds, e.g. if we say it's a TaskResumable kind but we failed to bind Action.
    //
    // We do NOT expect the procedure to be marked as bad in the face of such problems
    // and indeed we purposefully don't even return a boolean for whether we had any.
    // Similarly, even if a lambda body is bad (through lacking a return type), it should still be allowed to be
    // interpreted for return type inference

    if (GetLocalResumableKind() != ILTree::UnknownResumable)
    {
        VSFAIL("IndicateLocalResumable should be called exactly once, at the start of a method");
        return;
    }

    Type *underlyingReturnType = GetLocalReturnType();
    bool isAsyncKeywordUsed=false, isIteratorKeywordUsed=false;
    Location loc;
    Type *resumableGenericType = NULL;

    if (m_StatementLambdaInterpreter != NULL)
    {
        VSASSERT(m_StatementLambdaInterpreter->m_Tree, "StatementLambdaInterpreter lacks m_Tree");
        if (!m_StatementLambdaInterpreter->m_FunctionLambda) underlyingReturnType = TypeHelpers::GetVoidType(); // lambdas don't store "Sub" as VoidType. (instead they store it as "Class Void" which is different!!?!)
        isAsyncKeywordUsed = m_StatementLambdaInterpreter->m_IsAsyncKeywordUsed;
        isIteratorKeywordUsed = m_StatementLambdaInterpreter->m_IsIteratorKeywordUsed;
        loc = m_StatementLambdaInterpreter->m_Tree->Loc;
    }
    else if (m_Procedure != NULL)
    {
        if (underlyingReturnType==NULL) underlyingReturnType = TypeHelpers::GetVoidType(); // procedures store "NULL" as their return type...
        isAsyncKeywordUsed = m_Procedure->IsAsyncKeywordUsed();
        isIteratorKeywordUsed = m_Procedure->IsIteratorKeywordUsed();
        if (m_Procedure->HasLocation()) loc = *m_Procedure->GetLocation();
        else loc = Location::GetHiddenLocation();
    }
    else
    {
        // leave it as Unknown
        MarkContainingLambdaOrMethodBodyBad();
        return;
    }

    if (underlyingReturnType != NULL && underlyingReturnType->IsGenericTypeBinding())
    {
        // In case it was IEnumerable<T>, IEnumerator<T>, Task<T>, we'll pick up that T for later.
        // (in case it was some other generic, well, it does not harm to pick it up now as well...)
        GenericTypeBinding *gtb = underlyingReturnType->PGenericTypeBinding();
        if (gtb->GetArgumentCount() == 1) resumableGenericType = gtb->GetArgument(0);
    }


    if (!isAsyncKeywordUsed && !isIteratorKeywordUsed)
    {
        SetLocalResumableInfo(ILTree::NotResumable, NULL);
        return;
    }

    if (isAsyncKeywordUsed && isIteratorKeywordUsed)
    {
        // The parser alows both 'Async' and 'Iterator' modifiers together -- it's our job to report errors
        ReportSemanticError(ERRID_InvalidAsyncIteratorModifiers, loc);
        MarkContainingLambdaOrMethodBodyBad();
        // But we leave it as Unknown
        return;
    }

    if (isIteratorKeywordUsed)
    {
        Type *GenericIEnumerable = GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIEnumerableType) ? GetFXSymbolProvider()->GetType(FX::GenericIEnumerableType) : NULL;
        Type *GenericIEnumerator = GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIEnumeratorType) ? GetFXSymbolProvider()->GetType(FX::GenericIEnumeratorType) : NULL;
        Type *IEnumerable = GetFXSymbolProvider()->IsTypeAvailable(FX::IEnumerableType) ? GetFXSymbolProvider()->GetType(FX::IEnumerableType) : NULL;
        Type *IEnumerator = GetFXSymbolProvider()->IsTypeAvailable(FX::IEnumeratorType) ? GetFXSymbolProvider()->GetType(FX::IEnumeratorType) : NULL; 
    
        if (underlyingReturnType == NULL)
        {
            // This is typical of lambda type inference scenarios
            SetLocalResumableInfo(ILTree::IteratorResumable, NULL);
            return;
        }
        else if (underlyingReturnType->IsBad())
        {
            // An error has already been reported. We'll mark the method as an Iterator, so that Yield statements work
            // Actually, although the error has been reported, it hasn't necessarily been reported in this error table
            // e.g. "Iterator Function f() As bad" -- our own error table doesn't show anything yet.
            MarkContainingLambdaOrMethodBodyBad();

            SetLocalResumableInfo(ILTree::IteratorResumable, NULL);
            return;
        }
        else if (underlyingReturnType == IEnumerator ||
            (underlyingReturnType->IsGenericBinding() && underlyingReturnType->PGenericBinding()->GetGeneric() == GenericIEnumerator))
        {
            SetLocalResumableInfo(ILTree::IteratorResumable, resumableGenericType);
            return;
        }
        else if (underlyingReturnType == IEnumerable ||
                 (underlyingReturnType->IsGenericBinding() && underlyingReturnType->PGenericBinding()->GetGeneric() == GenericIEnumerable))
        {
            SetLocalResumableInfo(ILTree::IterableResumable, resumableGenericType);
            return;
        }
        else if (IEnumerable == NULL || IEnumerator == NULL || GenericIEnumerable == NULL || GenericIEnumerator == NULL)
        {
            ReportSemanticError(ERRID_BadIteratorReturn, loc);
            MarkContainingLambdaOrMethodBodyBad();
            // If we failed to find IEnumerable or IEnumerator, report error.
            ReportMissingType(IEnumerable == NULL? FX::IEnumerableType : FX::IEnumeratorType, loc);
            SetLocalResumableInfo(ILTree::IteratorResumable, NULL);
            return;
        }
        else
        {
            ReportSemanticError(ERRID_BadIteratorReturn, loc);
            MarkContainingLambdaOrMethodBodyBad();
            // But we'll still mark it as Iterator, so that Yield statements work
            SetLocalResumableInfo(ILTree::IteratorResumable, NULL);
            return;
        }
    }


    if (isAsyncKeywordUsed)
    {
        Type *Task = GetFXSymbolProvider()->IsTypeAvailable(FX::TaskType) ? GetFXSymbolProvider()->GetType(FX::TaskType) : NULL;
        Type *GenericTask = GetFXSymbolProvider()->IsTypeAvailable(FX::GenericTaskType) ? GetFXSymbolProvider()->GetType(FX::GenericTaskType) : NULL;

        bool req1 = GetFXSymbolProvider()->IsTypeAvailable(FX::AsyncTaskMethodBuilderType);
        bool req2 = GetFXSymbolProvider()->IsTypeAvailable(FX::GenericAsyncTaskMethodBuilderType);
        bool req3 = GetFXSymbolProvider()->IsTypeAvailable(FX::AsyncVoidMethodBuilderType);
        bool req4 = GetFXSymbolProvider()->IsTypeAvailable(FX::INotifyCompletionType);
        bool req5 = GetFXSymbolProvider()->IsTypeAvailable(FX::ICriticalNotifyCompletionType);
        bool req6 = GetFXSymbolProvider()->IsTypeAvailable(FX::IAsyncStateMachineType);
        bool reqs = Task!=NULL && GenericTask!=NULL && req1 && req2 && req3 && req4 && req5 && req6;

        if (underlyingReturnType == NULL)
        {
            // This is typical of lambda type inference scenarios
            SetLocalResumableInfo(ILTree::TaskResumable, NULL);
            return;
        }
        else if (underlyingReturnType->IsBad())
        {
            // An error has already been reported. We'll mark the method as Task, so that Await expressions and Return statements work

            // Actually, although the error has been reported, it hasn't necessarily been reported in this error table
            // e.g. "Async Function f() As bad" -- our own error table doesn't show anything yet.
            MarkContainingLambdaOrMethodBodyBad();

            SetLocalResumableInfo(ILTree::TaskResumable, NULL);
            return;
        }
        else if (underlyingReturnType->IsVoidType() ||
                 underlyingReturnType == Task ||
                 (underlyingReturnType->IsGenericBinding() && underlyingReturnType->PGenericBinding()->GetGeneric() == GenericTask))
        {
            if (!reqs)
            {
                ReportSemanticError(ERRID_AwaitLibraryMissing, loc); // "Are you missing a reference to AwaitCtpLibrary.dll ?"
                MarkContainingLambdaOrMethodBodyBad();
            }

            SetLocalResumableInfo(underlyingReturnType->IsVoidType() ? ILTree::SubResumable : ILTree::TaskResumable, resumableGenericType);
            return;
        }
        else if (!reqs)
        {
            ReportSemanticError(ERRID_AwaitLibraryMissing, loc); // "Are you missing a reference to AwaitCtpLibrary.dll ?"
            MarkContainingLambdaOrMethodBodyBad();

            // But still say it's a TaskResumable just to avoid further error reporting
            SetLocalResumableInfo(ILTree::TaskResumable, NULL);
            return;
        }
        else
        {
            ReportSemanticError(ERRID_BadAsyncReturn, loc);
            MarkContainingLambdaOrMethodBodyBad();
            SetLocalResumableInfo(ILTree::TaskResumable, NULL);
            return;
        }
    }

    VSFAIL("Internal error: we should not have fallen through to the end of IndicateLocalResumable");
    ReportSemanticError(ERRID_InternalCompilerError, loc);
    MarkContainingLambdaOrMethodBodyBad();
    return;

}

ILTree::Expression *
Semantics::InterpretBinaryOperation
(
    ParseTree::Expression::Opcodes Opcode,
    const Location &ExpressionLocation,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    ExpressionFlags Flags
)
{
    // all calls to InterpretBinaryOperation(ParseTree:Expression::*,..) are from code other
    // than generated expression during Select statement optimization.
    return
        InterpretBinaryOperation(
            MapOperator(Opcode),
            ExpressionLocation,
            Left,
            Right,
            Flags,
            false /*fSelectGenerated*/);
}


bool
Semantics::IsNothingOrConversionFromNothing
(
    ILTree::Expression *pExp
)
{
    AssertIfTrue(pExp == NULL);

    ILTree::ILNode *pCastOp = NULL;
    for (pCastOp = pExp;
        pCastOp->bilop == SX_CTYPE || pCastOp->bilop == SX_DIRECTCAST || pCastOp->bilop == SX_TRYCAST;
        pCastOp = pCastOp->AsExpressionWithChildren().Left)
    {
        if (IsBad(pCastOp))
        {
            return false;
        }
        
        if (!TypeHelpers::IsNullableType(pCastOp->AsExpression().ResultType) && 
           !TypeHelpers::IsRootObjectType(pCastOp->AsExpression().ResultType))
        {
            return false;
        }
    }
    
    if ((pCastOp->bilop == SX_NOTHING) &&
       TypeHelpers::IsReferenceType(pCastOp->AsExpression().ResultType))
    {
        return true;
    }
    else 
    {
        return false;
    }
}


ILTree::Expression *
Semantics::InterpretBinaryOperation
(
    BILOP BoundOpcode,
    const Location &ExpressionLocation,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    ExpressionFlags Flags,
    bool fSelectGenerated
)
{
    if (IsBad(Left) || IsBad(Right))
    {
        return AllocateBadExpression(ExpressionLocation);
    }


    if (IsNothingLiteral(Left) || IsNothingLiteral(Right))
    {
        if (IsNothingLiteral(Left) && IsNothingLiteral(Right))
        {
            // Comparing Nothing and Nothing succeeds, and operations
            // that provide an explicit type succeed.
            // And and Or succeed with a result type of Integer.
            // Everything else is rejected.
            //
            // The only reason these matter is for conditional compilation
            // expressions that refer to undefined constants.

            switch (BoundOpcode)
            {
                case SX_CONC:
                case SX_LIKE:

                    Right = Convert(Right, GetFXSymbolProvider()->GetStringType(), ExprNoFlags, ConversionWidening);
                    break;

                case SX_ORELSE:
                case SX_ANDALSO:

                    Right = Convert(Right, GetFXSymbolProvider()->GetBooleanType(), ExprNoFlags, ConversionWidening);
                    break;

                case SX_IS:
                case SX_ISNOT:
                case SX_EQ:
                case SX_NE:
                case SX_LT:
                case SX_LE:
                case SX_GE:
                case SX_GT:
                case SX_ADD:
                case SX_MUL:
                case SX_DIV:
                case SX_SUB:
                case SX_POW:
                case SX_IDIV:
                case SX_SHIFT_LEFT:
                case SX_SHIFT_RIGHT:
                case SX_MOD:

                    // Treating the operation as if its operands are integers
                    // gives correct results.

                    Right = Convert(Right, GetFXSymbolProvider()->GetIntegerType(), ExprNoFlags, ConversionWidening);
                    break;

                case SX_OR:
                case SX_AND:
                case SX_XOR:

                    Right = Convert(Right, GetFXSymbolProvider()->GetIntegerType(), ExprNoFlags, ConversionWidening);
                    break;

                default:
                    VSFAIL("unexpected binary operator");
                    ReportSemanticError(
                        ERRID_InternalCompilerError,
                        ExpressionLocation);

                    return AllocateBadExpression(ExpressionLocation);
            }
        }

        if (IsNothingLiteral(Left))
        {
            Type *OperandType = Right->ResultType;

            switch (BoundOpcode)
            {
                case SX_CONC:
                case SX_LIKE:
                    if (TypeHelpers::IsIntrinsicOrEnumType(OperandType) ||
                        TypeHelpers::IsCharArrayRankOne(OperandType) ||
                        TypeHelpers::IsDBNullType(OperandType, m_CompilerHost) ||
                        (
                            TypeHelpers::IsNullableType(OperandType, GetCompilerHost()) &&
                            TypeHelpers::IsIntrinsicOrEnumType(TypeHelpers::GetElementTypeOfNullable(OperandType, GetCompilerHost()))
                        ))
                    {
                        // For & and Like, a Nothing operand is typed String unless the other operand
                        // is non-intrinsic (VSW#240203).
                        // The same goes for DBNull (VSW#278518)
                        // The same goes for enum types (VSW#288077)
                        OperandType = GetFXSymbolProvider()->GetStringType();
                    }
                    break;

                case SX_SHIFT_LEFT:
                case SX_SHIFT_RIGHT:
                    // Nothing should default to Integer for Shift operations.
                    OperandType = GetFXSymbolProvider()->GetIntegerType();
                    break;
            }

            Left = Convert(Left, OperandType, ExprNoFlags, ConversionWidening);
        }
        else if (IsNothingLiteral(Right))
        {
            Type *OperandType = Left->ResultType;

            switch (BoundOpcode)
            {
                case SX_CONC:
                case SX_LIKE:
                    if (TypeHelpers::IsIntrinsicOrEnumType(OperandType) ||
                        TypeHelpers::IsCharArrayRankOne(OperandType) ||
                        TypeHelpers::IsDBNullType(OperandType, m_CompilerHost) ||
                        (
                            TypeHelpers::IsNullableType(OperandType, GetCompilerHost()) &&
                            TypeHelpers::IsIntrinsicOrEnumType(TypeHelpers::GetElementTypeOfNullable(OperandType, GetCompilerHost()))
                        ))
                    {
                        // For & and Like, a Nothing operand is typed String unless the other operand
                        // is non-intrinsic (VSW#240203).
                        // The same goes for DBNull (VSW#278518)
                        // The same goes for enum types (VSW#288077)
                        OperandType = GetFXSymbolProvider()->GetStringType();
                    }
                    break;
            }

            Right = Convert(Right, OperandType, ExprNoFlags, ConversionWidening);
        }
    }

    if ((BoundOpcode == SX_CONC &&
         ((!TypeHelpers::IsDBNullType(Left->ResultType, m_CompilerHost) &&
               TypeHelpers::IsDBNullType(Right->ResultType, m_CompilerHost)) ||
          (TypeHelpers::IsDBNullType(Left->ResultType, m_CompilerHost) &&
               !TypeHelpers::IsDBNullType(Right->ResultType, m_CompilerHost)))) ||
        (BoundOpcode == SX_ADD &&
         ((TypeHelpers::IsStringType(Left->ResultType) &&
               TypeHelpers::IsDBNullType(Right->ResultType, m_CompilerHost)) ||
          (TypeHelpers::IsDBNullType(Left->ResultType, m_CompilerHost) &&
               TypeHelpers::IsStringType(Right->ResultType)))))
    {
        if (TypeHelpers::IsDBNullType(Left->ResultType, m_CompilerHost))
        {
            Left = ProduceStringConstantExpression(NULL, 0, Left->Loc IDE_ARG(0));
        }

        if (TypeHelpers::IsDBNullType(Right->ResultType, m_CompilerHost))
        {
            Right = ProduceStringConstantExpression(NULL, 0, Right->Loc IDE_ARG(0));
        }
    }

    // For comparison operators, the result type computed here is not
    // the result type of the comparison (which is typically boolean),
    // but is the type to which the operands are to be converted. For
    // other operators, the type computed here is both the result type
    // and the common operand type.

    bool ResolutionFailed = false;
    Procedure *OperatorMethod = NULL;
    GenericBinding *OperatorMethodGenericContext = NULL;
    bool LiftedNullable = false;


    Type *ResultType =
        ResolveBinaryOperatorResultType(
            BoundOpcode,
            ExpressionLocation,
            Left,
            Right,
            ResolutionFailed,
            OperatorMethod,
            OperatorMethodGenericContext,
            LiftedNullable);



    VSASSERT(OperatorMethodGenericContext == NULL || OperatorMethod, "Generic context for null operator method unexpected");

    VSASSERT(OperatorMethod == NULL ||
             TypeHelpers::EquivalentTypes(ResultType, TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext)), "types don't match");

    if (ResolutionFailed)
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    Type *OperandType = ResultType;
    Type *OperationResultType = OperandType;
    ExpressionFlags ConversionFlags = Flags & ExprMustBeConstant;

    if (OperatorMethod)
    {
        ILTree::Expression * Result = NULL;
        // The binary operation resolves to a user-defined operator.
        if (BoundOpcode == SX_ANDALSO || BoundOpcode == SX_ORELSE)
        {
            // If operator resolution resolved to an applicable user-defined And/Or operator, and we are in the
            // context of short-circuiting, then we will generate the special form for overloaded AndAlso/OrElse.
            Result =
                InterpretUserDefinedShortCircuitOperator(
                    BoundOpcode,
                    OperatorMethod,
                    OperatorMethodGenericContext,
                    ExpressionLocation,
                    Left,
                    Right,
                    ConversionFlags);
        }
        else
        {
            Result =
                InterpretUserDefinedOperator(
                    BoundOpcode,
                    OperatorMethod,
                    OperatorMethodGenericContext,
                    ExpressionLocation,
                    Left,
                    Right,
                    ConversionFlags);
        }

        if (! IsBad(Result) && OperatorMethod->IsLiftedOperatorMethod())
        {

            // If OperatorMethodGenericContext is defined then it must be GenericTypeBinding
            ThrowIfFalse(OperatorMethodGenericContext == NULL || OperatorMethodGenericContext->IsGenericTypeBinding());

            GenericTypeBinding* pGenericTypeBinding = OperatorMethodGenericContext != NULL ? 
                OperatorMethodGenericContext->PGenericTypeBinding() : 
                NULL;

            if
            (

                !
                (
                    IsValidInLiftedSignature(OperatorMethod->GetType(), pGenericTypeBinding) &&
                    IsValidInLiftedSignature(OperatorMethod->GetFirstParam()->GetType(),pGenericTypeBinding) &&
                    IsValidInLiftedSignature(OperatorMethod->GetFirstParam()->GetNext()->GetType(), pGenericTypeBinding)
                )
            )
            {
                MakeBad(Result);
                ReportSemanticError
                (
                    ERRID_BinaryOperands3,
                    Result->Loc,
                    m_Compiler->OperatorToString(OperatorMethod->PLiftedOperatorMethod()->GetActualProc()->GetAssociatedOperatorDef()->GetOperator()),
                    Left->ResultType,
                    Right->ResultType
                );
            }
        }

        if (!IsBad(Result) &&
            (BoundOpcode == SX_EQ || BoundOpcode == SX_NE) &&
            OperatorMethod->IsMethodDecl() &&
            OperatorMethod->PMethodDecl()->IsLiftedOperatorMethod() &&
            Result->bilop == SX_CALL && 
                (IsNothingOrConversionFromNothing(Result->AsCallExpression().Right->AsExpressionWithChildren().Left) || 
                IsNothingOrConversionFromNothing(Result->AsCallExpression().Right->AsExpressionWithChildren().Right)))
        {
            ReportSemanticError(
                BoundOpcode == SX_EQ ? WRNID_EqualToLiteralNothing : WRNID_NotEqualToLiteralNothing,
                ExpressionLocation);
        }     

        return Result;
    }


    // Option Strict disallows all operations on Object operands. Or, at least, warn.
    if (m_UsingOptionTypeStrict)
    {
        bool MadeError = false;

        if (TypeHelpers::IsRootObjectType(Left->ResultType))
        {
            const Location &ErrorLocation = ExpressionLocation.ContainsInclusive(&Left->Loc) ?
                Left->Loc :
                ExpressionLocation;

            ReportSemanticError(
                BoundOpcode == SX_EQ || BoundOpcode == SX_NE ?
                    ERRID_StrictDisallowsObjectComparison1 :
                    ERRID_StrictDisallowsObjectOperand1,
                ErrorLocation,
                BoundOpcode);

            MadeError = true;
        }

        if (TypeHelpers::IsRootObjectType(Right->ResultType))
        {
            const Location &ErrorLocation = ExpressionLocation.ContainsInclusive(&Right->Loc) ?
                Right->Loc :
                ExpressionLocation;

            ReportSemanticError(
                BoundOpcode == SX_EQ || BoundOpcode == SX_NE ?
                    ERRID_StrictDisallowsObjectComparison1 :
                    ERRID_StrictDisallowsObjectOperand1,
                ErrorLocation,
                BoundOpcode);

            MadeError = true;
        }

        if (MadeError)
        {
            return AllocateBadExpression(ExpressionLocation);
        }
    }
    // warn if option strict is off
    else if (WarnOptionStrict())
    {
        if (!fSelectGenerated || BoundOpcode != SX_ORELSE) // avoid warning overinflation in Select/Case tatements
        {
            ERRID errid = fSelectGenerated ?
                            WRNID_ObjectMathSelectCase :
                            ((BoundOpcode == SX_EQ || BoundOpcode == SX_NE) ?
                                (BoundOpcode == SX_EQ ? WRNID_ObjectMath1 : WRNID_ObjectMath1Not) :
                                WRNID_ObjectMath2);

            if (TypeHelpers::IsRootObjectType(Left->ResultType))
            {
                ReportSemanticError(errid, Left->Loc, BoundOpcode);
            }

            if (TypeHelpers::IsRootObjectType(Right->ResultType))
            {
                ReportSemanticError(errid, Right->Loc, BoundOpcode);
            }
        }
    }

    // Concatenation will apply conversions to its operands as if the
    // conversions were explicit. Effectively, the use of the concatenation
    // operator is treated as an explicit conversion to String.
    // 
    if (BoundOpcode == SX_CONC)
    {
        AssertIfTrue(LiftedNullable);
        ConversionFlags |= ExprHasExplicitCastSemantics;

        if(TypeHelpers::IsStringType(OperandType))
        {
            if(TypeHelpers::IsNullableType(Right->ResultType, GetCompilerHost()))
            {
                //AssertIfFalse(TypeHelpers::IsStringType(OperandType));
                Right = ForceLiftToEmptyString(Right, OperandType);
            }
            if (TypeHelpers::IsNullableType(Left->ResultType, GetCompilerHost()))
            {
                //AssertIfFalse(TypeHelpers::IsStringType(OperandType));
                Left = ForceLiftToEmptyString(Left, OperandType);
            }
        }
    }


    Left = ConvertWithErrorChecking(Left, OperandType, ConversionFlags);

    // Perform special processing of the << and >> operators when not applied to System.Object.
    if (IsShiftOperator(BoundOpcode) && !TypeHelpers::IsRootObjectType(OperandType))
    {
        Type *SourceType = Right->ResultType;
        bool IsSourceNullable = TypeHelpers::IsNullableType(SourceType, m_CompilerHost);

        if (!GetFXSymbolProvider()->IsTypeAvailable(FX::GenericNullableType))
        {
            ReportMissingType(FX::GenericNullableType, ExpressionLocation);
            return AllocateBadExpression(ExpressionLocation);
        }

        Type *TargetType = LiftedNullable ?
            GetFXSymbolProvider()->GetNullableIntrinsicSymbol(t_i4) :
                GetFXSymbolProvider()->GetIntegerType();

        // If operator is lifted and operand is nullable, convert the operand to Nullable(Of Integer).
        // In all other cases convert the operand to Integer type.
        Right = ConvertWithErrorChecking
        (
            Right,
            IsSourceNullable || m_IsGeneratingXML ? TargetType : GetFXSymbolProvider()->GetIntegerType(),
            ConversionFlags
        );

        if
        (
            !IsBad(Right) &&
            (
                !m_IsGeneratingXML
                ||
                (
                    AllowsCompileTimeOperations(ResultType) &&
                    AllowsCompileTimeOperations(Left->ResultType) &&
                    AllowsCompileTimeOperations(Right->ResultType)
                )
            )
        )
        {
            // Calculate bitwise And mask based on operator result type.
            Vtypes OperandVtype = LiftedNullable ?
                TypeHelpers::GetElementTypeOfNullable(OperandType, m_CompilerHost)->GetVtype() :
                OperandType->GetVtype();

            int SizeMask = GetShiftSizeMask(OperandVtype);

            // Apply the mask to the operand.
            Right =
                InterpretBinaryOperation(
                    SX_AND,
                    Right->Loc,
                    Right,
                    ProduceConstantExpression(
                        SizeMask,
                        Right->Loc,
                        GetFXSymbolProvider()->GetIntegerType()
                        IDE_ARG(0)),
                    Flags,
                    fSelectGenerated);

            // If operator is lifted and operand is not, convert operand to Nullable(Of Integer).
            if (!IsBad(Right) && LiftedNullable && !IsSourceNullable)
            {
                Right = ConvertWithErrorChecking(Right, TargetType, ConversionFlags);
            }
        }
    }
    else
    {
        Right = ConvertWithErrorChecking(Right, OperandType, ConversionFlags);
    }

    if (IsBad(Left) || IsBad(Right))
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    unsigned ResultFlags = LiftedNullable ? SXF_OP_LIFTED_NULLABLE : 0;
    bool PreventCompileTimeEvaluation = false;
    bool ApplyIsTrue = false;

    // Check for special cases.
    switch (BoundOpcode)
    {
        case SX_ADD:
        {
            if (TypeHelpers::IsStringType(ResultType))
            {
                // Transform the addition into a string concatenation.  This won't use a runtime helper - it will turn into System.String::Concat
                BoundOpcode = SX_CONC;
            }
            break;
        }

        case SX_LIKE:
            PreventCompileTimeEvaluation = true;
            __fallthrough;

        case SX_EQ:
        case SX_NE:
        case SX_LE:
        case SX_GE:
        case SX_LT:
        case SX_GT:
        {
            if ((TypeHelpers::IsRootObjectType(OperandType) || TypeHelpers::IsStringType(OperandType)) &&
                 (m_SourceFileOptions & OPTION_OptionText) &&
                 !m_EvaluatingConditionalCompilationConstants)      // ignore Option Text in conditional compilation(b112186)
            {
                AssertIfTrue(LiftedNullable);
                SetFlag32(ResultFlags, SXF_RELOP_TEXT);
                PreventCompileTimeEvaluation = true;
            }

            if (!TypeHelpers::IsRootObjectType(ResultType) ||
                (HasFlag(Flags, ExprIsOperandOfConditionalBranch) && BoundOpcode != SX_LIKE))
            {
                if (LiftedNullable && !OperatorMethod)
                {               
                    if ( (BoundOpcode == SX_EQ || BoundOpcode == SX_NE) &&
                         (IsNothingOrConversionFromNothing(Left) || IsNothingOrConversionFromNothing(Right)))
                    {
                        ReportSemanticError(
                            BoundOpcode == SX_EQ ? WRNID_EqualToLiteralNothing : WRNID_NotEqualToLiteralNothing,
                            ExpressionLocation);
                    }
                    
                    AssertIfTrue(TypeHelpers::IsRootObjectType(ResultType));
                    if (!GetFXSymbolProvider()->IsTypeAvailable(FX::GenericNullableType))
                    {
                        ReportMissingType(FX::GenericNullableType, ExpressionLocation);
                        return AllocateBadExpression(ExpressionLocation);
                    }

                    if (HasFlag(Flags, ExprIsOperandOfConditionalBranch))
                    {
                        ApplyIsTrue = true;
                        ResultType = GetFXSymbolProvider()->GetBooleanType();
                        OperationResultType = GetCompilerHost()->GetFXSymbolProvider()->GetNullableIntrinsicSymbol(t_bool);
                    }
                    else
                    {
                        ResultType = GetCompilerHost()->GetFXSymbolProvider()->GetNullableIntrinsicSymbol(t_bool);
                        OperationResultType = ResultType;
                    }
                }
                else
                {
                    ResultType = GetFXSymbolProvider()->GetBooleanType();
                    OperationResultType = ResultType;
                }
            }

            break;
        }
    }

    ILTree::Expression *Result = NULL;

    if (IsConstant(Left) && IsConstant(Right))
    {
        AssertIfTrue(LiftedNullable);
        if (AllowsCompileTimeOperations(ResultType) &&
            AllowsCompileTimeOperations(Left->ResultType) &&
            AllowsCompileTimeOperations(Right->ResultType) &&
            !PreventCompileTimeEvaluation &&
            (!m_IsGeneratingXML || HasFlag(Flags, ExprMustBeConstant) || BoundOpcode != SX_CONC))
        {
            VSASSERT(ResultType == OperationResultType, "Binary expression interpretation type pun.");
            Result =
                PerformCompileTimeBinaryOperation(
                    BoundOpcode,
                    OperationResultType,
                    ExpressionLocation,
                    Left,
                    Right);
        }
        else if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                ExpressionLocation);

            return AllocateBadExpression(ExpressionLocation);
        }
    }
    else
    {
        VSASSERT(!HasFlag(Flags, ExprMustBeConstant), "Required constant isn't.");
    }

    if (Result == NULL)
    {
        Result =
            AllocateExpression(
                BoundOpcode,
                OperationResultType,
                Left,
                Right,
                ExpressionLocation);

        SetFlag32(Result, ResultFlags);

        if (ApplyIsTrue)
        {
            Result = AllocateExpression
            (
                SX_ISTRUE,
                GetFXSymbolProvider()->GetBooleanType(),
                Result,
                ExpressionLocation
            );
        }
    }

    Result = Convert(Result, ResultType, ExprNoFlags, ConversionWidening);

    // 



    return Result;
}

/*=======================================================================================
CheckRecursiveOperatorCall

Given the current context and an operator being called, warn the user if the call is
recurisve.  A recurisve operator call is most likely unintentional.
=======================================================================================*/
void
Semantics::CheckRecursiveOperatorCall
(
    Procedure *CallTarget,
    const Location &CallLocation
)
{
    VSASSERT(CallTarget && CallTarget->IsUserDefinedOperatorMethod(), "expected operator method");

    if (CallTarget == m_Procedure &&
        CallTarget->IsUserDefinedOperatorMethod())
    {
        ReportSemanticError(
            WRNID_RecursiveOperatorCall,
            CallLocation,
            CallTarget->GetAssociatedOperatorDef()->GetName());
    }
}

/*=======================================================================================
InterpretUserDefinedOperator

Given an operator method and operands, do the necessary conversions and build a bound
tree representing a call to the overloaded operator.  The right operand can be NULL in
the case of a unary operator call.
=======================================================================================*/
ILTree::Expression *
Semantics::InterpretUserDefinedOperator
(
    BILOP Opcode,
    Procedure *OperatorMethod,
    GenericBinding *OperatorMethodGenericContext,
    const Location &ExpressionLocation,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    ExpressionFlags Flags
)
{
    ILTree::Expression *RightArgument = NULL;
    bool SomeOperandsBad = false;

    if (Right)
    {
        VSASSERT(IsBinaryOperator(OperatorMethod->GetAssociatedOperatorDef()->GetOperator()),
                 "expected binary operator");

        Type *RightType =
            TypeInGenericContext(OperatorMethod->GetFirstParam()->GetNext()->GetType(), OperatorMethodGenericContext);

        if (TypeHelpers::IsBadType(RightType))
        {
            ReportBadType(RightType, ExpressionLocation);
            SomeOperandsBad = true;
        }

        // We must optimize the concat here because the normal route has been subverted by the user-defined operator.
        // Rather than the left and right operands being joined by a normal concatenation, they have been "split" and thus
        // require seperate optimization.
        //
        if (!IsBad(Right) &&
            Right->bilop == SX_CONC &&
            Right->vtype == t_string &&  // Note (8/24/2001):  Only optimize for string concatenations.  Object concatenation is complicated by DBNull.
            !m_IsGeneratingXML)
        {
            Right = OptimizeConcatenate(Right, Right->Loc);
        }

        // Convert the Right operand to the second parameter type of the operator method.
        Right =
            ConvertWithErrorChecking(
                Right,
                RightType,
                Flags);

        RightArgument =
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                Right,
                ExpressionLocation);
    }
    else
    {
        VSASSERT(IsUnaryOperator(OperatorMethod->GetAssociatedOperatorDef()->GetOperator()),
                 "expected unary operator");
    }

    Type *LeftType =
        TypeInGenericContext(OperatorMethod->GetFirstParam()->GetType(), OperatorMethodGenericContext);

    if (TypeHelpers::IsBadType(LeftType))
    {
        ReportBadType(LeftType, ExpressionLocation);
        SomeOperandsBad = true;
    }

    // We must optimize the concat here because the normal route has been subverted by the user-defined operator.
    // Rather than the left and right operands being joined by a normal concatenation, they have been "split" and thus
    // require seperate optimization.
    //
    if (!IsBad(Left) &&
        Left->bilop == SX_CONC &&
        Left->vtype == t_string &&  // HACK (8/24/2001):  Only optimize for string concatenations.  Object concatenation is complicated by DBNull.
        !m_IsGeneratingXML)
    {
        Left = OptimizeConcatenate(Left, Left->Loc);
    }

    // Convert the Left operand to the first parameter type of the operator method.
    Left =
        ConvertWithErrorChecking(
            Left,
            LeftType,
            Flags);

    ILTree::Expression *MethodReference =
        ReferToSymbol(
            ExpressionLocation,
            OperatorMethod,
            chType_NONE,
            NULL,
            OperatorMethodGenericContext,
            ExprIsExplicitCallTarget);
    SetFlag32(MethodReference, SXF_SYM_NONVIRT);  // All operators are shared.

    if (IsBad(MethodReference))
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    if (TypeHelpers::IsBadType(OperatorMethod->GetType()))
    {
        ReportBadType(OperatorMethod->GetType(), ExpressionLocation);
        SomeOperandsBad = true;
    }

    ILTree::Expression *Result =
        AllocateExpression(
            SX_CALL,
            TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext),
            MethodReference,
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                Left,
                RightArgument,
                ExpressionLocation),
            ExpressionLocation);

    if (SomeOperandsBad)
    {
        MakeBad(Result);
    }

    if (!IsBad(Result))
    {
        CheckObsolete(OperatorMethod->GetAssociatedOperatorDef(), ExpressionLocation);
        CheckRecursiveOperatorCall(OperatorMethod, ExpressionLocation);
    }

    if( Opcode != SX_COUNT )
    {
        SetFlag32(Result, SXF_CALL_WAS_OPERATOR);
        Result->AsCallExpression().OperatorOpcode = Opcode;
    }

    return Result;
}

ILTree::Expression *
Semantics::InterpretUserDefinedOperator
(
    BILOP Opcode,
    Procedure *OperatorMethod,
    GenericBinding *OperatorMethodGenericContext,
    const Location &ExpressionLocation,
    ILTree::Expression *Operand,
    ExpressionFlags Flags
)
{
    return
        InterpretUserDefinedOperator(
            Opcode,
            OperatorMethod,
            OperatorMethodGenericContext,
            ExpressionLocation,
            Operand,
            NULL,
            Flags);
}

/*=======================================================================================
InterpretUserDefinedShortCircuitOperator

This function builds a bound tree representing an overloaded short circuiting expression
after determining that the necessary semantic conditions are met.

An expression of the form:

    x AndAlso y  (where the type of x is X and the type of y is Y)

is an overloaded short circuit operation if X and Y are user-defined types and an
applicable operator And exists after applying normal operator resolution rules.

Given an applicable And operator declared in type T, the following must be true:

    - The return type and parameter types must be T.
    - T must contain a declaration of operator IsFalse.

If these conditions are met, the expression "x AndAlso y" is translated into:

    !T.IsFalse(temp = x) ? T.And(temp, y) : temp

The temporary is necessary for evaluating x only once. Similarly, "x OrElse y" is
translated into:

    !T.IsTrue(temp = x) ? T.Or(temp, y) : temp










*/
ILTree::Expression *
Semantics::InterpretUserDefinedShortCircuitOperator
(
    BILOP Opcode,
    Procedure *OperatorMethod,
    GenericBinding *OperatorMethodGenericContext,
    const Location &ExpressionLocation,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    ExpressionFlags Flags
)
{
    VSASSERT(Opcode == SX_ANDALSO || Opcode == SX_ORELSE, "Unexpected short circuit operator!!!");

    Type *OperatorType = TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext);

    if (!TypeHelpers::EquivalentTypes(
            OperatorType,
            TypeInGenericContext(OperatorMethod->GetFirstParam()->GetType(), OperatorMethodGenericContext)) ||
        !TypeHelpers::EquivalentTypes(
            OperatorType,
            TypeInGenericContext(OperatorMethod->GetFirstParam()->GetNext()->GetType(), OperatorMethodGenericContext)))
    {
        ReportSemanticError(
            ERRID_UnacceptableLogicalOperator3,
            ExpressionLocation,
            OperatorMethod->GetAssociatedOperatorDef(),
            OperatorMethodGenericContext ? (BCSYM *)OperatorMethodGenericContext : OperatorMethod->GetContainingClass(),
            Opcode == SX_ANDALSO ? m_Compiler->TokenToString(tkANDALSO) : m_Compiler->TokenToString(tkORELSE));

        return AllocateBadExpression(ExpressionLocation);
    }

    // Convert the operands to the operator type.
    Left = ConvertWithErrorChecking(Left, OperatorType, ExprNoFlags);
    Right = ConvertWithErrorChecking(Right, OperatorType, ExprNoFlags);

    if (IsBad(Left) || IsBad(Right))
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    // Now we need the IsTrue/IsFalse condition operator, so bind to it.
    bool ResolutionFailed = false;
    bool ResolutionIsLateBound = false;
    Procedure *ConditionOperator = NULL;
    GenericBinding *ConditionOperatorGenericContext = NULL;

    bool PreviouslyReportingErrors = m_ReportErrors;
    m_ReportErrors = false;

    Type *ConditionType =
        ResolveUserDefinedOperator(
            Opcode,
            ExpressionLocation,
            Left,
            ResolutionFailed,
            ResolutionIsLateBound,
            ConditionOperator,
            ConditionOperatorGenericContext);

    m_ReportErrors = PreviouslyReportingErrors;

    AssertIfTrue(ResolutionIsLateBound);

    // T must contain a declaration of operator IsTrue/IsFalse.
    if (ResolutionFailed ||
        ResolutionIsLateBound)
    {
        ReportSemanticError(
            ERRID_ConditionOperatorRequired3,
            ExpressionLocation,
            OperatorType,
            Opcode == SX_ANDALSO ?
                m_Compiler->OperatorToString(OperatorIsFalse) :
                m_Compiler->OperatorToString(OperatorIsTrue),
            Opcode == SX_ANDALSO ?
                m_Compiler->TokenToString(tkANDALSO) :
                m_Compiler->TokenToString(tkORELSE));

        return AllocateBadExpression(ExpressionLocation);
    }

    if
    (
        !(
            TypeHelpers::EquivalentTypes(ConditionType, GetFXSymbolProvider()->GetBooleanType()) ||
            (
                TypeHelpers::IsNullableType(ConditionType, m_CompilerHost) &&
                TypeHelpers::EquivalentTypes(
                    TypeHelpers::GetElementTypeOfNullable(ConditionType, m_CompilerHost),
                    GetFXSymbolProvider()->GetBooleanType())
            )
        ) ||
        !TypeHelpers::EquivalentTypes
        (
            OperatorType,
            TypeInGenericContext(ConditionOperator->GetFirstParam()->GetType(), ConditionOperatorGenericContext)
        )
    )
    {
        // This shouldn't happen since all IsTrue/IsFalse operators must take an operand of the
        // containing class's type and return a Boolean.  Fail gracefully otherwise.

        ReportSemanticError(
            ERRID_BinaryOperands3,
            ExpressionLocation,
            Opcode,
            Left->ResultType,
            Right->ResultType);

        return AllocateBadExpression(ExpressionLocation);
    }

    // At this point, we have fully verified the operators, bound the
    // correct operator method, and converted the Left and Right types
    // to match the operator. Build a binop node now, and store this information
    // for the lowering phase.

    ILTree::Expression* Result = AllocateUserDefinedOperatorExpression(
        Opcode,
        OperatorType,
        Left,
        Right,
        ExpressionLocation);

    Result->AsUserDefinedBinaryOperatorExpression().OperatorMethod = OperatorMethod;
    Result->AsUserDefinedBinaryOperatorExpression().OperatorMethodContext = OperatorMethodGenericContext;
    Result->AsUserDefinedBinaryOperatorExpression().InterpretationFlags = Flags;
    Result->AsShortCircuitBooleanOperatorExpression().ConditionOperator = ConditionOperator;
    Result->AsShortCircuitBooleanOperatorExpression().ConditionOperatorContext = ConditionOperatorGenericContext;

    return( Result );
}

ILTree::Expression *
Semantics::SynthesizeMeReference
(
    const Location &ReferringLocation,
    Type *ReferencedClassOrInterface,
    bool SuppressMeSynthesis,
    bool ReportError,
    unsigned * pErrorID
)
{
    //For simplicity, I didn't write the code to deal with null pErrorID when ReportError is false.
    //If you need to suppor this then you need to add extra null checks to the places where pErrorID is dereferenced.
    ThrowIfFalse(ReportError || pErrorID);

    if (!SuppressMeSynthesis)
    {
        if (m_Procedure )
        {
            ClassOrRecordType *ReferencingClass = ContainingClass();
            // Verify that the reference originates from a class that is or is
            // derived from the referenced class.

            if (IsOrInheritsFromOrImplements(ReferencingClass, ReferencedClassOrInterface))
            {
                if (WithinSharedProcedure())
                {
                    if (ReportError)
                    {
                        ReportSemanticError
                        (
                            ERRID_BadInstanceMemberAccess,
                            ReferringLocation
                        );
                    }
                    else
                    {
                        *pErrorID = ERRID_BadInstanceMemberAccess;
                    }

                    return AllocateBadExpression(ReferringLocation);
                }
                else if (m_DisallowMeReferenceInConstructorCall)
                {
                    if (ReportError)
                    {
                        ReportSemanticError
                        (
                            ERRID_InvalidImplicitMeReference,
                            ReferringLocation
                        );
                    }
                    else
                    {
                        *pErrorID = ERRID_InvalidImplicitMeReference;
                    }
                    // Don't mark this expression bad so we can continue analyzing it.
                }

                ILTree::Expression *Result =
                    AllocateSymbolReference(
                        ReferencingClass->GetMe(),
                        ReferencingClass,
                        NULL,
                        ReferringLocation);

                // Create a generic type binding that includes the parameters of the generic type as arguments.

                if (IsGenericOrHasGenericParent(ReferencingClass))
                {
                    GenericBinding *binding = NULL;

                    if ( !binding )
                    {
                        binding = SynthesizeOpenGenericBinding(ReferencingClass, m_SymbolCreator);
                    }

                    Result->AsSymbolReferenceExpression().GenericBindingContext = binding;
                    Result->ResultType = binding;
                }

                return Result;
            }
        }
    }

    if (ReportError)
    {
        ReportSemanticError
        (
            ERRID_ObjectReferenceNotSupplied,
            ReferringLocation
        );
    }
    else
    {

        *pErrorID = ERRID_ObjectReferenceNotSupplied;
    }

    return AllocateBadExpression(ReferringLocation);
}

ILTree::Expression *
Semantics::InterpretDelegateBinding
(
    ILTree::Expression *Input,
    Type *DelegateType,
    const Location &AddressOfTextSpan,
    bool SuppressMethodNameInErrorMessages,
    ExpressionFlags Flags,
    //I had to change the name of the "DelegateRelaxationLevel" variable to be "RelaxationLevel"
    //because C++ is ----. In particular, it allows a variable to have the same name as a type,
    //but then reinterprets all instances of that name in that scope to be the variable not the type.
    //This was preventing me from declaraing other variables of this type.
    DelegateRelaxationLevel &RelaxationLevel,
    bool *pRequiresNarrowingConversion
)
{


    ILTree::Expression * pMethodOperand = Input->AsExpressionWithChildren().Left;

    return
        InterpretDelegateBinding
        (
            pMethodOperand,
            Input,
            DelegateType,
            AddressOfTextSpan,
            SuppressMethodNameInErrorMessages,
            Flags,
            RelaxationLevel,
            OvrldNoFlags,
            pRequiresNarrowingConversion
        );
}

// A creation of a delegate binding is logically of the form:
//
//     New DelegateClass (AddressOf ClassOrInstance.Method)
//
// This turns into a call to a delegate constructor that takes two arguments:
//     1) The object to use in the binding. (Nothing if the method is shared.)
//     2) A reference to the method to use in the binding, typed as a
//        pointer-sized integer.

ILTree::Expression *
Semantics::InterpretDelegateBinding
(
    ILTree::Expression *MethodOperand,
    ILTree::Expression *Input,
    Type *DelegateType,
    const Location &AddressOfTextSpan,
    bool SuppressMethodNameInErrorMessages,
    ExpressionFlags Flags,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    OverloadResolutionFlags OverloadFlags,
    bool *pRequiresNarrowingConversion
)
{
    MethodConversionClass MethodConversion = MethodConversionIdentity;

    // Find the Invoke method of the delegate class.
    Declaration *InvokeMethod = GetInvokeFromDelegate(DelegateType, m_Compiler);

    //EXTMET - 
    if (InvokeMethod == NULL || IsBad(InvokeMethod) || !InvokeMethod->IsProc())
    {
        if (m_ReportErrors)
        {
            StringBuffer DelegateRepresentation;
            DelegateType->GetBasicRep(m_Compiler, m_Procedure == NULL ? NULL : m_Procedure->GetContainingClass(), &DelegateRepresentation, NULL);

            ReportSemanticError
            (
                ERRID_UnsupportedMethod1,
                MethodOperand->Loc,
                DelegateRepresentation
            );
        }

        return AllocateBadExpression(AddressOfTextSpan);
    }

    // Check the Invoke method for bad parameter types.  We cannot continue delegate binding
    // without eliminating a potentially bad Invoke method (VSW#172753).

    for (Parameter *Param = InvokeMethod->PProc()->GetFirstParam();
         Param;
         Param = Param->GetNext())
    {
        if (TypeHelpers::IsBadType(Param->GetType()))
        {
            ReportBadType(Param->GetType(), AddressOfTextSpan);

            return AllocateBadExpression(AddressOfTextSpan);
        }
    }

    if (InvokeMethod->PProc()->GetType() && TypeHelpers::IsBadType(InvokeMethod->PProc()->GetType()))
    {
        ReportBadType(InvokeMethod->PProc()->GetType(), AddressOfTextSpan);

        return AllocateBadExpression(AddressOfTextSpan);
    }

    if (IsLateReference(MethodOperand))
    {
        MethodConversion = MethodConversionLateBoundCall;
        DelegateRelaxationLevel = max(DetermineDelegateRelaxationLevel(MethodConversion), DelegateRelaxationLevel);

        return
            CreateRelaxedDelegateLambda
            (
                MethodOperand,
                NULL,
                NULL, // InstanceMethod Proc Definition
                InvokeMethod->PProc(),// Delegate Proc definition
                DelegateType,
                NULL,
                AddressOfTextSpan,
                false //this is not an extension call...late bound extension calls are not supported
            );
    }

    GenericBinding *GenericBindingContext = NULL;

    bool ResultIsExtensionMethod = false;

    Procedure *MatchingMember =
        ResolveMethodForDelegateInvoke
        (
            MethodOperand,
            Input->uFlags,
            InvokeMethod->PProc(),
            DelegateType,
            DelegateType, // Original delegate type for error messages
            GenericBindingContext,
            SuppressMethodNameInErrorMessages,
            false, //IgnoreReturnValueErrorsForInference
            MethodConversion,
            OverloadFlags,
            ResultIsExtensionMethod,
            pRequiresNarrowingConversion
        );

    if (MatchingMember == NULL)
    {
        return AllocateBadExpression(AddressOfTextSpan);
    }

    if
    (
        GetFXSymbolProvider()->IsTypeAvailable(FX::GenericNullableType) &&
        !ResultIsExtensionMethod
    )
    {
        if (MatchingMember->GetParent() == GetFXSymbolProvider()->GetType(FX::GenericNullableType))
        {
            if (MatchingMember->OverriddenProcLast())
            {
                // If the method bound to is a Nullable method that is overriding a base method, then
                // do not give the Nullable error, but instead allow binding to the base method.

                MatchingMember = MatchingMember->OverriddenProcLast();
                GenericBindingContext = NULL;
            }
            else
            {
                ReportSemanticError(ERRID_AddressOfNullableMethod, MethodOperand->Loc);
                return AllocateBadExpression(AddressOfTextSpan);
            }
        }
    }


    //Setting DelegateRelaxationLevel. This will be used by
    //overload resolution in case of conflict. This is to prevent applications that compiled in VB8
    //to fail in VB9 because there are more matches. And the same for flipping strict On to Off.
    DelegateRelaxationLevel = max(DetermineDelegateRelaxationLevel(MethodConversion), DelegateRelaxationLevel);

    Procedure *DelegateBinding = ViewAsProcedure(MatchingMember);
    ILTree::Expression *ObjectArgument;

    if (MethodOperand->bilop == SX_OVERLOADED_GENERIC)
    {
        MethodOperand = MethodOperand->AsOverloadedGenericExpression().BaseReference;
    }

    // If the bound member is overloaded, then we need to make a special check here for
    // obsoleteness and availability because the normal channels through CheckAcessibility
    // have been averted.

    //However, we do not want to do the obsolete check if the list of candidates includes extension methods because we
    //will do the obsolete check inside extension method overlaod resolution.
    if
    (
        MethodOperand->bilop != SX_EXTENSION_CALL &&
        ViewAsProcedure(MethodOperand->AsSymbolReferenceExpression().Symbol)->IsOverloads()
    )
    {
        CheckObsolete(DelegateBinding, MethodOperand->Loc);

        if (!DeclarationIsAvailableToCurrentProject(DelegateBinding))
        {
            if (m_ReportErrors)
            {
                StringBuffer TextBuffer1;

                //EXTMET - 
                ReportSmartReferenceError(
                    ERRID_SymbolFromUnreferencedProject3,
                    m_Project,
                    DelegateBinding->GetContainingProject(),
                    m_Compiler,
                    m_Errors,
                    DelegateBinding->GetContainingProject()->GetFileName(),     // 
                    &MethodOperand->Loc,
                    ExtractErrorName(DelegateBinding, TextBuffer1),
                    GetErrorProjectName(DelegateBinding->GetContainingProject()),
                    GetErrorProjectName(m_Project));
            }

            return AllocateBadExpression(AddressOfTextSpan);
        }
    }

    // Prepare the arguments for the delegate constructor.

    // Port SP1 CL 2955581 to VS10
    bool isStubRequiredForMethodConversion = IsStubRequiredForMethodConversion(MethodConversion);

    if (MethodOperand->bilop == SX_EXTENSION_CALL)
    {

        ILTree::ExtensionCallExpression * pExtensionCall = &MethodOperand->AsExtensionCallExpression();
        ObjectArgument = pExtensionCall->ImplicitArgumentList->AsExpressionWithChildren().Left->AsArgumentExpression().Left;

        if (ResultIsExtensionMethod || !MatchingMember->IsShared())
        {
            if (pExtensionCall->ImplicitMeErrorID)
            {
                ReportSemanticError
                (
                    pExtensionCall->ImplicitMeErrorID,
                    ObjectArgument->Loc
                );
                ObjectArgument = MakeBad(ObjectArgument);
            }
            else if (! IsBad(ObjectArgument))
            {
                ObjectArgument = MakeRValue(ObjectArgument);
            }
        }
        else
        {
            if (!ObjectArgument->NameCanBeType && (m_Procedure != NULL && ! m_Procedure->IsSyntheticMethod()) && !DelegateBinding->IsSyntheticMethod() && ! HasFlag32(MethodOperand, SXF_EXTENSION_CALL_ME_IS_SYNTHETIC))
            {
                ReportSemanticError(
                    WRNID_SharedMemberThroughInstance,
                    MethodOperand->Loc);
            }

            ObjectArgument =
                AllocateExpression
                (
                    SX_NOTHING,
                    GetFXSymbolProvider()->GetObjectType(),
                    MethodOperand->Loc
                );
        }

        if
        (
            ! pExtensionCall->ImplicitMeErrorID &&
            TypeHelpers::IsValueTypeOrGenericParameter(ObjectArgument->ResultType)
        )
        {
           // first check boxing of restricted types
            if (ObjectArgument->vtype == t_struct && m_ReportErrors)
            {
                CheckRestrictedType(
                    ERRID_RestrictedConversion1,
                    ObjectArgument->ResultType->DigThroughAlias(),
                    &MethodOperand->Loc,
                    m_CompilerHost,
                    m_Errors);
            }
        }

        if (IsBad(ObjectArgument))
        {
            return AllocateBadExpression(Input->Loc);
        }
    }
    else if (DelegateBinding->IsShared())
    {
        ObjectArgument =
            AllocateExpression(
            SX_NOTHING,
            GetFXSymbolProvider()->GetObjectType(),
            MethodOperand->Loc);

        if (MethodOperand->AsSymbolReferenceExpression().BaseReference && !MethodOperand->AsSymbolReferenceExpression().BaseReference->NameCanBeType &&
            m_Procedure != NULL && !m_Procedure->IsSyntheticMethod() &&
            !DelegateBinding->IsSyntheticMethod())
        {
            ReportSemanticError(
                WRNID_SharedMemberThroughInstance,
                MethodOperand->Loc);
        }
    }
    else
    {
        ObjectArgument = MethodOperand->AsSymbolReferenceExpression().BaseReference;
        Type *MethodOwner = DelegateBinding->GetParent();

        if (ObjectArgument == NULL)
        {
            // References to Me are not allowed in the arguments of a constructor call
            // if that call is the first statement in another constructor.
            // Unfortunately, the Me sythesizing here doesn't occur until after the
            // arguments are fully analyzed and the state information is no longer valid.
            // Therefore, a flag in the SX_ADDRESSOF node tells us when to disallow Me references.

            bool OriginalStateValue = m_DisallowMeReferenceInConstructorCall;
            if (HasFlag32(Input->uFlags, SXF_DISALLOW_ME_REFERENCE))
            {
                m_DisallowMeReferenceInConstructorCall = true;
            }

            ObjectArgument =
                SynthesizeMeReference(
                    MethodOperand->Loc,
                    MethodOwner,
                    HasFlag32(MethodOperand, SXF_SYM_MAKENOBASE));

            m_DisallowMeReferenceInConstructorCall = OriginalStateValue;

            if (IsBad(ObjectArgument))
            {
                return AllocateBadExpression(AddressOfTextSpan);
            }
        }
        else if (ObjectArgument->bilop == SX_SYM &&
                 HasFlag32(ObjectArgument, (SXF_SYM_MYBASE | SXF_SYM_MYCLASS)) &&
                 DelegateBinding->IsMustOverrideKeywordUsed())
        {
            // AddressOf expressions of the form "MyBase.Member" are invalid if the member is
            // abstract.  Such calls are guaranteed to fail at run time.

            ReportSemanticError(
                HasFlag32(ObjectArgument, SXF_SYM_MYBASE) ? ERRID_MyBaseAbstractCall1 : ERRID_MyClassAbstractCall1,
                MethodOperand->Loc,
                DelegateBinding);

            return AllocateBadExpression(AddressOfTextSpan);
        }

        if (TypeHelpers::IsValueTypeOrGenericParameter(ObjectArgument->ResultType))
        {
            // Delegates can be bound only to instances of reference types.
            // Box the base reference.

           // first check boxing of restricted types
            if (ObjectArgument->vtype == t_struct && m_ReportErrors)
            {
                CheckRestrictedType(
                    ERRID_RestrictedConversion1,
                    ObjectArgument->ResultType->DigThroughAlias(),
                    &MethodOperand->Loc,
                    m_CompilerHost,
                    m_Errors);
            }

            // Port SP1 CL 2955581 to VS10
            // 



            if (!isStubRequiredForMethodConversion)
            {
                ObjectArgument =
                    AllocateExpression(
                        SX_CTYPE,
                        GetFXSymbolProvider()->GetObjectType(),
                        ObjectArgument,
                        ObjectArgument->Loc);
            }
        }
    }

    // Warn if delegate relaxation drops the returned "Task" from an async function
    if (isStubRequiredForMethodConversion)
    {
        bool targetIsAsync = MatchingMember->IsAsyncKeywordUsed();
        bool targetInSameCompilationUnit = MatchingMember->GetContainingProject() == m_ContainingClass->GetContainingProject();
        bool suppressWarning = HasFlag(Flags, ( ExprCreateDelegateInstance | ExprIsExplicitCast));

        if (targetIsAsync && targetInSameCompilationUnit && !suppressWarning)
        {
            Location loc = !MethodOperand->Loc.IsHidden() ? MethodOperand->Loc : *MatchingMember->GetLocation();
            // in the case of "Handles" keywords, the MethodOperand location is hidden
            ReportSemanticError(WRNID_UnobservedAwaitableDelegate, loc);
        }
    }

    // Port SP1 CL 2955581 to VS10
    if (isStubRequiredForMethodConversion)
    {
        // Temporary set sourcefile and option strict when evaluating the lambda expression if we are creating one for
        // the handles clause to the location fo the handles method instead of the
        BackupValue<SourceFile*> backup_SourceFile(&m_SourceFile);
        BackupValue<bool> backup_UsingOptionTypeStrict(&m_UsingOptionTypeStrict);

        SourceFile * SourceFile = HasFlag32(Input->uFlags, SXF_USE_STRICT_OF_TARGET_METHOD) ? MatchingMember->GetSourceFile() : m_SourceFile;
        m_SourceFile = SourceFile;
        m_UsingOptionTypeStrict = SourceFile ? SourceFile->GetOptionFlags() & OPTION_OptionStrict : true; // SourceFile may be NULL

        ILTree::Expression* RelaxedDelegateLambdaStub = CreateRelaxedDelegateLambda(
            MethodOperand,
            ObjectArgument,
            MatchingMember, // InstanceMethod Proc Definition
            InvokeMethod->PProc(),// Delegate Proc definition
            DelegateType,
            GenericBindingContext,
            AddressOfTextSpan,
            ResultIsExtensionMethod);

        if (IsBad(RelaxedDelegateLambdaStub))
        {
            return AllocateBadExpression(AddressOfTextSpan);
        }
        else
        {
            if (HasFlag32(Input->uFlags, SXF_USED_IN_REMOVEHANDLER))
            {
                ReportSemanticError(
                    WRNID_RelDelegatePassedToRemoveHandler,
                    Input->Loc);
            }
            return RelaxedDelegateLambdaStub;
        }
    }

    unsigned DelegateCreateFlags = 0;

    if (MethodOperand->bilop != SX_EXTENSION_CALL)
    {

        MethodOperand->AsSymbolReferenceExpression().Symbol = DelegateBinding;
        MethodOperand->AsSymbolReferenceExpression().BaseReference = NULL;
        MethodOperand->AsSymbolReferenceExpression().GenericBindingContext = GenericBindingContext;

        MethodOperand->Loc = AddressOfTextSpan;
    }
    else
    {
        if (ResultIsExtensionMethod)
        {
            DelegateCreateFlags |= SXF_CALL_WAS_EXTENSION_CALL;
        }

        MethodOperand =
            AllocateSymbolReference
            (
                DelegateBinding,
                TypeHelpers::GetVoidType(),
                NULL,
                AddressOfTextSpan,
                GenericBindingContext
            );

        // Port SP1 CL 2954860 to VS10
        // The CLR doesn't support creating curried delegates that close over a ByRef 'this' argument.
        // What we need to do when the first argument (which specifies the 'this' argument to the
        // delegate) is ByRef is create a lamdba to do the call for us, e.g.

        // 'We are going to create a curried delegate that closes over the byref string argument
        // Delegate Function D(ByVal y As Integer) As Integer
        // ' Dim x As D = AddressOf "Hello".Foo will be morphed to essentially
        // ' Dim x As D = Function(y As Integer) "Hello".Foo(y)
        // <Extension()> _
        // Function Foo(ByRef x As String, ByVal y As Integer) As Integer   
        //      'The delegate to this extension method needs to close over the ByRef 'this' argument:
        //      ByRef x as string
        // End Function 
        //
        // A similar problem/solution exists when the 'this' argument to the delegate2 is a value type,
        // so we deal with that case below, as well.

        BCSYM_Param *FirstParam = DelegateBinding->GetFirstParam();
        bool IsObjectParamByRef = FirstParam != NULL && FirstParam->IsByRefKeywordUsed() ? true : false;

        if (ResultIsExtensionMethod && (IsObjectParamByRef ||
                TypeHelpers::IsValueTypeOrGenericParameter(ObjectArgument->ResultType)))
        {
            ILTree::Expression * pRet =
                CreateExtensionMethodValueTypeDelegateLambda
                (
                    DelegateType,
                    ObjectArgument,
                    MethodOperand,
                    ViewAsProcedure(InvokeMethod),
                    AddressOfTextSpan
                );

            return pRet;
        }
    }

    return CreateDelegateConstructorCall(
        DelegateType,
        ObjectArgument,
        MethodOperand,
        DelegateCreateFlags,
        AddressOfTextSpan);
}


ILTree::Expression *
Semantics::CreateExtensionMethodValueTypeDelegateLambda
(
    Type * DelegateType,
    ILTree::Expression * ObjectArgument,
    ILTree::Expression * MethodOperand,
    BCSYM_Proc * InvokeMethod,
    const Location & AddressOfTextSpan
)
{
    ParserHelper ph(&m_TreeStorage);

    ParseTree::ParameterList * pParameters = NULL;
    ParseTree::ArgumentList * pArguments = NULL;
    ParseTree::ParameterList * pLastParameter= NULL;
    ParseTree::ArgumentList * pLastArgument = NULL;

    // Port SP1 CL 2954860 to VS10
    // Microsoft: We want to capture the objectargument into a short lived temporary so that
    // for expressions like x.Prop().Foo() we lift x.Prop(), and not just x.

    Variable * var = NULL;
    ILTree::Expression * captured = NULL;
    
    captured = CaptureInShortLivedTemporary(ObjectArgument, var);

    pArguments = pLastArgument  =
        ph.AddArgument
        (
            NULL,
            ph.CreateBoundExpression(
                AllocateSymbolReference(
                    var,
                    var->GetType(),
                    NULL,
                    AddressOfTextSpan,
                    NULL)),
            AddressOfTextSpan
        );

    // Port SP1 CL 2923807 to VS10
    // Build the list of parameters for the lambda and the list of arguments for the delegate call

    // Port SP1 CL 2941411 to VS10
    unsigned ParamSuffix = 0;
    STRING *ParamName = STRING_CONST(m_Compiler, Param); // i.e. "Param"

    for (BCSYM_Param * pParam = InvokeMethod->GetFirstParam(); pParam; pParam = pParam->GetNext())
    {
        // Port SP1 CL 2941411 to VS10
        // Name the params we build $Param1, $Param2, etc.
        StringBuffer buf;
        buf.AppendPrintf(L"%s%s%u", LAMBDA_PARAMETER_PREFIX, ParamName, ParamSuffix++);
        STRING *LambdaParamName = m_Compiler->AddString(&buf);

        // Port SP1 CL 2929782 to VS10
        // Now we pick up the specifiers. ByRef and ByVal are valid specifiers. Optional and ParamArray
        // are currently disallowed for delegates. If this changes in future, we might have to call
        // ph.AppendToParameterList for them.
        VSASSERT(!pParam->IsOptional() && !pParam->IsParamArray(), L"Didn't expect an Optional or ParamArray parameter for a delegate");
        //
        // Note that the CreateLambdaExpression will assume all specifiers are "ByVal" unless they're explicitly
        // declared ByRef. So we only specify in the ByRef case.
        ParseTree::ParameterSpecifierList *Specifiers = NULL;
        if (pParam->IsByRefKeywordUsed())
        {
            Specifiers =
                ph.CreateParameterSpecifierList
                (
                    AddressOfTextSpan,
                    1,
                    ph.CreateParameterSpecifier(ParseTree::ParameterSpecifier::ByRef, AddressOfTextSpan)
                );
        }

        // The 'this' argument of the delegate, e.g. given d = AddressOf "Hello".Foo(), this is "Hello".
        pLastArgument = 
            ph.AddArgument
            (
                pLastArgument,
                ph.CreateNameExpression
                (
                    1,
                    LambdaParamName
                ),
                AddressOfTextSpan
            );

        // These are pulled from the parameters of the delegate definition and become the parameters
        // of the lambda, e.g.: Delegate Function D(ByVal DelegateY as Integer) as Integer this is
        // DelegateY -> Function (DelegateY as integer)
        pLastParameter = 
            ph.AddParameter
            (
                pLastParameter,
                ph.CreateParameter
                (
                    ph.CreateIdentifierDescriptor
                    (
                        LambdaParamName,
                        AddressOfTextSpan
                    ),
                    NULL, AddressOfTextSpan, false, false, Specifiers
                ),
                AddressOfTextSpan
            );

        if (! pParameters)
        {
            pParameters = pLastParameter;
        }
    }

    GenericTypeBinding *DelegateBindingContext = TypeHelpers::IsGenericTypeBinding(DelegateType) ? DelegateType->PGenericTypeBinding() : NULL;

    bool functionDelegate = false;

    // Check if the delegate returns a value.

    BCSYM_Proc *pDelegateProc = GetInvokeFromDelegate(DelegateType, m_Compiler)->PProc();
    Type *DelegateReturnType = pDelegateProc->GetType();
    DelegateReturnType = ReplaceGenericParametersWithArguments(DelegateReturnType, DelegateBindingContext, m_SymbolCreator);
    functionDelegate = DelegateReturnType != NULL && !DelegateReturnType->IsVoidType();

    ParseTree::LambdaExpression *lambdaParseTree = 
            ph.CreateSingleLineLambdaExpression(
                pParameters,
                ph.CreateMethodCall(
                    ph.CreateBoundExpression( MethodOperand ),
                    pArguments, 
                    AddressOfTextSpan), 
                AddressOfTextSpan,
                functionDelegate);

    // Note: this used to call the full InterpretExpression. But everywhere else (i.e. in QuerySemantics) we never
    // call InterpretExpression: we just go straight to InterpretLambdaExpression. I think that's the correct thing
    // to do here.
    ILTree::Expression *Call = InterpretLambdaExpression(lambdaParseTree, ExprDontInferResultType | ExprSkipOverloadResolution);
    
    Call = ConvertWithErrorChecking(Call, DelegateType, ExprNoFlags);

    SetFlag32(Call, SXF_CALL_WAS_EXTENSION_CALL);

    // Build the SX_SEQ_OP2.

    return AllocateExpression(
        SX_SEQ_OP2,
        Call->ResultType,
        captured,
        Call,
        Call->Loc);
}

ILTree::Expression *
Semantics::CreateDelegateConstructorCall
(
    Type *DelegateType,
    ILTree::Expression *ObjectArgument,
    ILTree::Expression *MethodOperand,
    unsigned DelegateCreateFlags,
    const Location &AddressOfTextSpan
)
{
    // Find the appropriate delegate constructor, which will have
    // two parameters, the first of type Object and the second of type UIntPtr.
    Declaration *DelegateConstructor = DelegateType->PClass()->GetFirstInstanceConstructor(m_Compiler);

    while (DelegateConstructor && !IsMagicDelegateConstructor(ViewAsProcedure(DelegateConstructor)))
    {
        DelegateConstructor = DelegateConstructor->GetNextOverload();
    }

    if (DelegateConstructor == NULL)
    {
        ReportSemanticError(
            ERRID_DelegateConstructorMissing1,
            MethodOperand->Loc,
            DelegateType);
    }

    // Create the constructor call, fix up the New tree, and declare victory.
    ILTree::Expression *ConstructorCall =
        AllocateDelegateConstructorCall(
            TypeHelpers::GetVoidType(),
            ReferToSymbol(
                MethodOperand->Loc,
                DelegateConstructor,
                chType_NONE,
                NULL,
                TypeHelpers::IsGenericTypeBinding(DelegateType) ? DelegateType->PGenericTypeBinding() : NULL,
                ExprIsExplicitCallTarget | ExprIsConstructorCall),
            ObjectArgument,
            MethodOperand,
            AddressOfTextSpan);

    SetFlag32(ConstructorCall->AsDelegateConstructorCallExpression().Constructor, SXF_SYM_NONVIRT);
    SetFlag32(ConstructorCall, DelegateCreateFlags);

    ILTree::NewExpression *Result = &AllocateExpression(SX_NEW, DelegateType, AddressOfTextSpan)->AsNewExpression();
    Result->Class = DelegateType;
    Result->ConstructorCall = ConstructorCall;

    return Result;
}

void
Semantics::GetAccessibleSignatureMismatchErrorAndLocation
(
    unsigned long numberOfFormalTypeParameters,
    unsigned long numberOfActualTypeArguments,
    bool suppressMethodNameInErrorMessages,
    bool candidateIsExtensionMethod,
    Location * pMethodOperandLocation,
    Location * pActualTypeArgumentsSpan,
    RESID * pErrorIDOut,
    Location ** ppErrorLocationOut
)
{
    ThrowIfNull(pErrorIDOut);

    Location * pDummyLocation = NULL;

    if (!ppErrorLocationOut)
    {
        ppErrorLocationOut = & pDummyLocation;
    }

    if (!pActualTypeArgumentsSpan || pActualTypeArgumentsSpan->IsInvalid())
    {
        pActualTypeArgumentsSpan = pMethodOperandLocation;
    }

    if (numberOfFormalTypeParameters > numberOfActualTypeArguments)
    {
        *ppErrorLocationOut = pActualTypeArgumentsSpan;
        if (suppressMethodNameInErrorMessages)
        {
            *pErrorIDOut = ERRID_TooFewGenericArguments;
        }
        else if (candidateIsExtensionMethod)
        {
            *pErrorIDOut = ERRID_TooFewGenericArguments2;
        }
        else
        {
            *pErrorIDOut = ERRID_TooFewGenericArguments1;
        }
    }
    else if (numberOfFormalTypeParameters < numberOfActualTypeArguments)
    {
        *ppErrorLocationOut = pActualTypeArgumentsSpan;
        if (suppressMethodNameInErrorMessages)
        {
            *pErrorIDOut = ERRID_TooManyGenericArguments;
        }
        else if (candidateIsExtensionMethod)
        {
            if (numberOfFormalTypeParameters > 0)
            {
                *pErrorIDOut = ERRID_TooManyGenericArguments2;
            }
            else
            {
                *pErrorIDOut = ERRID_TypeOrMemberNotGeneric2;
            }
        }
        else
        {
            *pErrorIDOut = ERRID_TooManyGenericArguments1;
        }
    }
    else
    {
        *ppErrorLocationOut = pMethodOperandLocation;
        if (suppressMethodNameInErrorMessages)
        {
            *pErrorIDOut = ERRID_DelegateBindingMismatch;
        }
        else if (candidateIsExtensionMethod)
        {
            *pErrorIDOut = ERRID_DelegateBindingMismatch3_3;
        }
        else
        {
            //3_2 only takes 2 parameters, but it's second parameter is in position 3.
            //The name 3_2 is designed to indicate this. This makes the error compatable
            //with the other errors returned from this function.
            *pErrorIDOut = ERRID_DelegateBindingMismatch3_2;
        }
    }
}

Procedure *
Semantics::ResolveMethodForDelegateInvoke
(
    ILTree::Expression *MethodOperand,
    unsigned uFlags,
    Procedure *InvokeMethod,
    Type *DelegateType,
    Type* OriginalDelegateTypeForErrors,
    GenericBinding *&GenericBindingContext,
    bool SuppressMethodNameInErrorMessages,
    bool IgnoreReturnValueErrorsForInference,
    MethodConversionClass &MethodConversion,
    OverloadResolutionFlags OverloadFlags,
    bool & ResultIsExtensionMethod,
    bool *pRequiresNarrowingConversion
)
{
    GenericBindingContext = NULL;

    Procedure *InaccessibleMatchingMethod = NULL;
    bool MultipleAccessibleMethodsFound = false;
    bool SomeCandidatesBad = false;
    Location *MatchingMethodTypeArgumentLocations = NULL;

    bool candidatesAreExtensionMethods = MethodOperand->bilop == SX_EXTENSION_CALL;

    Procedure *MatchingMethod;
    if (candidatesAreExtensionMethods)
    {
        GenericBindingInfo binding = GenericBindingContext;

        MatchingMethod =
            ResolveExtensionMethodForDelegateInvokeTryingFullAndRelaxedArgs
            (
                &MethodOperand->AsExtensionCallExpression(),
                uFlags,
                InvokeMethod,
                DelegateType,
                OriginalDelegateTypeForErrors,
                IgnoreReturnValueErrorsForInference,
                binding,
                MethodConversion,
                ResultIsExtensionMethod,
                pRequiresNarrowingConversion
            );

        if (MatchingMethod)
        {
            binding.ConvertToFullBindingIfNecessary(this, MatchingMethod);
            GenericBindingContext = binding.PGenericBinding();
        }
    }
    else
    {
        MatchingMethod = ResolveInstanceMethodForDelegateInvokeTryingFullAndRelaxedArgs
        (
            MethodOperand,
            uFlags,
            InvokeMethod,
            DelegateType,
            OriginalDelegateTypeForErrors,
            InaccessibleMatchingMethod,
            SomeCandidatesBad,
            MatchingMethodTypeArgumentLocations,
            GenericBindingContext,
            SuppressMethodNameInErrorMessages,
            IgnoreReturnValueErrorsForInference,
            MethodConversion,
            OverloadFlags,
            pRequiresNarrowingConversion
        );
    }

    if (SomeCandidatesBad)
    {
        return NULL;
    }
    return MatchingMethod;
}


Procedure *
Semantics::ResolveCandidateInstanceMethodForDelegateInvokeAndReturnBindingContext
(
    ILTree::Expression *MethodOperand,
    unsigned uFlags,
    Procedure *InvokeMethod,
    Type *DelegateType,
    Type* OriginalDelegateTypeForErrors,
    Procedure *&InaccessibleMatchingMethod,
    bool &SomeCandidatesBad,
    Location *&MatchingMethodTypeArgumentLocations,
    GenericBinding * &GenericBindingContext,
    bool SuppressMethodNameInErrorMessages,
    bool IgnoreReturnValueErrorsForInference,
    MethodConversionClass &MethodConversion,
    OverloadResolutionFlags OverloadFlags,
    bool *pRequiresNarrowingConversion,
    bool *pWouldHaveSucceededWithStrictOff,
    bool *pCouldTryZeroArgumentRelaxation,
    bool AttemptZeroArgumentRelaxation
)
{
    ThrowIfTrue(MethodOperand->bilop == SX_EXTENSION_CALL);

    GenericBindingContext = NULL;
    bool MultipleAccessibleMethodsFound = false;

    Type **MethodTypeArguments = NULL;
    Location *MethodTypeArgumentLocations = NULL;
    unsigned MethodTypeArgumentCount = 0;
    GenericBinding *MethodBindingContext = NULL;
    MethodConversion = MethodConversionIdentity;

    // Get explicit type parameters & 'real' target method if present
    if (MethodOperand->bilop == SX_OVERLOADED_GENERIC)
    {
        MethodTypeArguments = MethodOperand->AsOverloadedGenericExpression().TypeArguments;
        MethodTypeArgumentLocations = MethodOperand->AsOverloadedGenericExpression().TypeArgumentLocations;
        MethodTypeArgumentCount = MethodOperand->AsOverloadedGenericExpression().TypeArgumentCount;

        MethodOperand = MethodOperand->AsOverloadedGenericExpression().BaseReference;
    }

    // If we've got a method binding already, that means we must not have an overloaded method.
    // However, since we want to leverage overload resolution, we're going to fake up the
    // method arguments here.
    if (MethodOperand->AsSymbolReferenceExpression().GenericBindingContext &&
        MethodOperand->AsSymbolReferenceExpression().GenericBindingContext->GetGeneric() == MethodOperand->AsSymbolReferenceExpression().Symbol)
    {
        VSASSERT(!MethodTypeArguments && !ViewAsProcedure(MethodOperand->AsSymbolReferenceExpression().Symbol)->IsOverloads(), "Unexpected!");

        MethodBindingContext = MethodOperand->AsSymbolReferenceExpression().GenericBindingContext;
        MethodTypeArguments = MethodBindingContext->GetArguments();
        MethodTypeArgumentCount = MethodBindingContext->GetArgumentCount();
        GenericBindingContext = MethodBindingContext->GetParentBinding();
    }
    else
    {
        GenericBindingContext = MethodOperand->AsSymbolReferenceExpression().GenericBindingContext;
    }

    GenericTypeBinding *DelegateBindingContext = TypeHelpers::IsGenericTypeBinding(DelegateType) ? DelegateType->PGenericTypeBinding() : NULL;

    Location AddressOfLocation = MethodOperand->Loc;
    Declaration *TargetMethod = MethodOperand->AsSymbolReferenceExpression().Symbol;
    ExpressionFlags BindingFlags = ExprNoFlags;
    bool ResolutionFailed = false;

    Procedure *TargetProcedure;
    // Build up an argument list based on the parameters of the delegate's Invoke method. No need if we are doing zero argument relaxation.
    ExpressionList *InvokeMethodArguments = NULL;

    if (!AttemptZeroArgumentRelaxation)
    {
        ILTree::Expression **NextArgumentInList  = &InvokeMethodArguments;
        for (Parameter *CurrentParameter = InvokeMethod->GetFirstParam();
               CurrentParameter;
               CurrentParameter = CurrentParameter->GetNext())
        {
            // Get the type of the parameter
            Type *ParameterType = GetDataType(CurrentParameter);

            if (ParameterType->IsPointerType())
            {
                ParameterType = ParameterType->PPointerType()->GetRoot();
            }

            ParameterType = ReplaceGenericParametersWithArguments(ParameterType, DelegateBindingContext, m_SymbolCreator);

            ILTree::Expression *TypeReference = AllocateExpression(
                SX_NAME,
                ParameterType,
                AddressOfLocation);

            // Build up an argument representing the parameter
            ExpressionList *CurrentArgument =
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    AllocateExpression(
                        SX_ARG,
                        TypeHelpers::GetVoidType(),
                        TypeReference,
                        AddressOfLocation),
                    AddressOfLocation);

            // Link the argument into the arglist we're building
            *NextArgumentInList = CurrentArgument;
            NextArgumentInList = &CurrentArgument->AsExpressionWithChildren().Right;
        }
    }

    // Resolve the generics for return type.
    // Keep it NULL such that the delegates returntype won't be taken part of in overload resolution when we are
    // inferring the returntype.
    BCSYM * InvokeMethodReturnType = IgnoreReturnValueErrorsForInference ? NULL : ReplaceGenericParametersWithArguments(
        InvokeMethod->GetType(),
        DelegateBindingContext,
        m_SymbolCreator);

    if (InvokeMethodArguments == NULL)
    {
        // No need to try zero argument relaxation if there are no arguments in the first place.
        *pCouldTryZeroArgumentRelaxation = false;
    }

    GenericBinding * GenericBindingContextBeforeOverload = GenericBindingContext;

    if (HasFlag(uFlags, SXF_TARGET_METHOD_RESOLVED))
    {
        TargetProcedure = ViewAsProcedure(TargetMethod);
    }
    else
    {
        bool ResolutionIsLateBound = false;
        bool ResolutionIsAmbiguous = false;

        OverloadFlags = OverloadFlags | OvrldIgnoreLateBound | OvrldDontReportSingletonErrors | OvrldReportErrorsForAddressOf |
            ((InvokeMethodReturnType == NULL || TypeHelpers::IsVoidType(InvokeMethodReturnType)) ? OvrldPreferSubOverFunction : OvrldPreferFunctionOverSub);

        // Delegate to overload resolution logic to determine best target method
        Declaration *TargetDeclaration = ResolveOverloadedCall
        (
            AddressOfLocation,
            TargetMethod,
            AttemptZeroArgumentRelaxation ? NULL : InvokeMethodArguments,
            OriginalDelegateTypeForErrors,
            InvokeMethodReturnType,
            GenericBindingContext,
            MethodTypeArguments,
            MethodTypeArgumentCount,
            BindingFlags,
            OverloadFlags,
            InstanceTypeOfReference(MethodOperand->AsSymbolReferenceExpression().BaseReference),
            ResolutionFailed,
            ResolutionIsLateBound,
            ResolutionIsAmbiguous
        );
        // We passed in "GenericBindingContext" the GenericTypeBinding of the type containing the first potential candidate.
        // This routine found the winning candidate. It returned in "GenericBindingContext" either the
        // GenericTypeBinding of the winning candidate (if it wasn't a generic method); or, the GenericMethodBinding
        // of the winning candidate (if it was); or, if no candidate won then due to our OvrldDontReportSingletonErrors
        // it will return back just our TargetMethod but without any changes to GenericBindingContext.
        //
        // A few lines below here, "if (IsGeneric(TargetProcedure))..." will get back the GenericTypeBinding of the winning
        // candidate in either of the above cases.

        if (ResolutionIsAmbiguous)
        {
            *pCouldTryZeroArgumentRelaxation = false;
        }

        TargetProcedure = TargetDeclaration ? ViewAsProcedure(TargetDeclaration) : NULL;
    }

    bool InferenceFailed = false;

    // Build a generic binding context & infer type arguments as necessary
    if (TargetProcedure && IsGeneric(TargetProcedure))
    {
        if (MethodBindingContext)
        {
            GenericBindingContext = MethodBindingContext;
        }
        else if (MethodTypeArguments)
        {
            // NB. At this stage GenericBindingContext is either the binding context for the winning candidate (if there was one),
            // or just the original GenericBindingContextBeforeOverload of the type containing the first candidate method.

            // Build a generic binding for the supplied type arguments
            Type *PossiblyGenericType = GenericBindingContextBeforeOverload ? GenericBindingContextBeforeOverload : TargetProcedure->GetContainer()->PNamedRoot();
            GenericTypeBinding *GenericTypeBinding = DeriveGenericBindingForMemberReference(PossiblyGenericType, TargetProcedure, m_SymbolCreator, m_CompilerHost);
            GenericBindingContext =
                m_SymbolCreator.GetGenericBinding(
                    false,
                    TargetProcedure,
                    MethodTypeArguments,
                    MethodTypeArgumentCount,
                    GenericTypeBinding);
            // DevDiv#151296: we don't mutate MethodOperand.GenericBindingContext here. That's left to some upstream caller.
        }
        else
        {
            ExpressionList *CopyOutArguments = NULL;
            bool SomeArgumentsBad = false;
            bool ArgumentArityBad = false;
            bool RequiresNarrowingConversion = false;
            bool RequiresSomeConversion = false;
            bool AllNarrowingIsFromObject = true;
            bool AllNarrowingIsFromNumericLiteral = true;
            bool AllFailedInferenceIsFromObject = true;
            DelegateRelaxationLevel DelegateRelaxationLevel = DelegateRelaxationLevelNone;
            TypeInferenceLevel TypeInferenceLevel = TypeInferenceLevelNone;
            bool RequiresUnwrappingNullable = false;
            bool RequiresInstanceMethodBinding = false;

            int errorsBefore = (m_ReportErrors && m_Errors) ? m_Errors->GetErrorCount() : 0;

            // NB. In the following call, we know that TargetProcedure is generic,
            // but GenericBindingContext might either be a binding for that TargetProcedure,
            // or it might merely be the context of the type containing it.
            // Either way, MatchArguments2 is robust against either possibility.

            // Infer type arguments
            MatchArguments2
            (
                AddressOfLocation,
                TargetProcedure,
                NULL,
                GenericBindingContext,
                InvokeMethodArguments,
                InvokeMethodReturnType,
                BindingFlags,
                OvrldReportErrorsForAddressOf,
                CopyOutArguments,
                false,
                false,
                false,
                false,
                SomeArgumentsBad,
                ArgumentArityBad,
                RequiresNarrowingConversion,
                RequiresSomeConversion,
                AllNarrowingIsFromObject,
                AllNarrowingIsFromNumericLiteral,
                InferenceFailed,
                AllFailedInferenceIsFromObject,
                SuppressMethodNameInErrorMessages,
                false, //the candidate is not an extension method
                DelegateRelaxationLevel,
                TypeInferenceLevel,
                RequiresUnwrappingNullable,
                RequiresInstanceMethodBinding
            );

            if (!InferenceFailed)
            {
                // DevDiv#151296: we don't mutate MethodOperand.GenericBindingContext here. That's left to some upstream caller.
            }

            int errorsAfter = (m_ReportErrors && m_Errors) ? m_Errors->GetErrorCount() : 0;

            // Dev10 #466405 We will bail out only if an error has in fact been reported or we are not reporting errors at all.
            if (SomeArgumentsBad && 
                    (!m_ReportErrors || errorsAfter > errorsBefore) )
            {
                // Fail, error has been reported, bail out.
                return NULL;
            }
        }
    }



    // Look for predefined CLR reference conversions in the right directions for the return type & input parameters
    if (TargetProcedure)
    {
        if (ResolutionFailed)
        {
            MethodConversion = MethodConversionError;
        }
        else
        {
            MethodConversion = ClassifyMethodConversion(
                TargetProcedure,
                GenericBindingContext,
                InvokeMethod,
                DelegateBindingContext,
                IgnoreReturnValueErrorsForInference,
                &m_SymbolCreator,
                false);
        }
    }

    SourceFile * SourceFile = HasFlag(uFlags, SXF_USE_STRICT_OF_TARGET_METHOD) ? TargetProcedure->GetSourceFile() : m_SourceFile;
    // Dev10#691957 - we may be called in cases where TargetProcedure comes from imported metadata,
    // and so SourceFile may be NULL. That's an internal compiler error. But we'll cover it with an ASSERT now
    // since there's no need to crash for what amounts to just a better choice of error message
    VSASSERT(SourceFile!=NULL, "Non-fatal internal error: shouldn't have used SXF_USE_STRICT_OF_TARGET_METHOD flag");

    *pWouldHaveSucceededWithStrictOff = false;
    // Conversion not allowed
    if (!IsSupportedMethodConversion(
            SourceFile ? SourceFile->GetOptionFlags() & OPTION_OptionStrict : true, // SourceFile may be NULL
            MethodConversion,
            pWouldHaveSucceededWithStrictOff,
            pRequiresNarrowingConversion,
            !HasFlag(uFlags, SXF_TARGET_METHOD_RESOLVED)) // Conversion is for AddressOf
        )
    {
        if (*pWouldHaveSucceededWithStrictOff)
        {
            *pCouldTryZeroArgumentRelaxation = false;
        }

        if (m_ReportErrors)
        {
            StringBuffer MethodRepresentation;
            StringBuffer DelegateRepresentation;
            TargetProcedure->GetBasicRep(
                m_Compiler,
                TargetProcedure->GetContainingClass(),
                &MethodRepresentation,
                GenericBindingContext);
            DelegateType->PNamedRoot()->GetBasicRep(
                m_Compiler,
                m_Procedure == NULL ? NULL : m_Procedure->GetContainingClass(),
                &DelegateRepresentation,
                OriginalDelegateTypeForErrors->IsGenericBinding() ? OriginalDelegateTypeForErrors->PGenericBinding() : NULL,
                NULL,
                true);

            // Method '|1' does not have a signature compatible with delegate '|2'.
            ReportSemanticError
            (
                *pWouldHaveSucceededWithStrictOff ? ERRID_DelegateBindingMismatchStrictOff2 : ERRID_DelegateBindingIncompatible2,
                AddressOfLocation,
                MethodRepresentation.GetString(),
                DelegateRepresentation.GetString()
            );
        }
        // Bail out since this isn't a legal target
        return NULL;
    }

    return TargetProcedure;
}


Procedure *
Semantics::ResolveInstanceMethodForDelegateInvokeTryingFullAndRelaxedArgs
(
    ILTree::Expression *MethodOperand,
    unsigned uFlags,
    Procedure *InvokeMethod,
    Type *DelegateType,
    Type* OriginalDelegateTypeForErrors,
    Procedure *&InaccessibleMatchingMethod,
    bool &SomeCandidatesBad,
    Location *&MatchingMethodTypeArgumentLocations,
    GenericBinding * &GenericBindingContext,
    bool SuppressMethodNameInErrorMessages,
    bool IgnoreReturnValueErrorsForInference,
    MethodConversionClass &MethodConversion,
    OverloadResolutionFlags OverloadFlags,
    bool *pRequiresNarrowingConversion
)
{
    Procedure *MatchingMethod = NULL;
    TemporaryErrorTable backup_error_table(m_Compiler, &m_Errors);
    const unsigned withArgumentsIndex = 0;
    const unsigned withOutArgumentsIndex = 1;

    if (m_Errors)
    {
        backup_error_table.AddTemporaryErrorTable(new ErrorTable(*m_Errors));
        m_Errors = backup_error_table.NewErrorTable(withArgumentsIndex);
    }

    // !! DANGER !!  See the comments in TemporaryErrorTable.Restore. MatchArguments can potentially
    // have side-effects on the SX_ tree as well (in particular in alters function(x)x by adding the
    // lambda parameter's "As" type). Using a temporary error table is all well and good, but what
    // do we do about a temporary SX_ tree? There's no mechanism for it.
    // Fortunately that doesn't cause problems here, since if our first attempt at resolution failed
    // but the zero-argument attempt did, then it means there were no arguments in the SX_ tree that
    // we might have mutated!

    Procedure* MatchingMethodWithArguments = NULL;
    GenericBinding* GenericBindingContextWithArguments = GenericBindingContext;
    MethodConversionClass MethodConversionWithArguments = MethodConversion;
    bool requiresNarrowingConversionWithArguments = pRequiresNarrowingConversion ? *pRequiresNarrowingConversion : false;
    bool someCandidatesBadWithArguments = false;
    bool wouldHaveSucceededWithStrictOffWithArguments = false;
    bool couldTryZeroArgumentRelaxation = true;


    MatchingMethodWithArguments = ResolveCandidateInstanceMethodForDelegateInvokeAndReturnBindingContext
    (
        MethodOperand,
        uFlags,
        InvokeMethod,
        DelegateType,
        OriginalDelegateTypeForErrors,
        InaccessibleMatchingMethod,
        someCandidatesBadWithArguments,
        MatchingMethodTypeArgumentLocations,
        GenericBindingContextWithArguments,
        SuppressMethodNameInErrorMessages,
        IgnoreReturnValueErrorsForInference,
        MethodConversionWithArguments,
        OverloadFlags,
        &requiresNarrowingConversionWithArguments,
        &wouldHaveSucceededWithStrictOffWithArguments,
        &couldTryZeroArgumentRelaxation,
        false // Don't use Zero-Argument Relaxation
    );

    bool ReportErrorsForWithArguments = false;

    if ((MatchingMethodWithArguments == NULL || someCandidatesBadWithArguments) &&
        couldTryZeroArgumentRelaxation)
    {
        if (m_Errors)
        {
            backup_error_table.AddTemporaryErrorTable(new ErrorTable(*backup_error_table.OldErrorTable()));
            m_Errors = backup_error_table.NewErrorTable(withOutArgumentsIndex);
        }

        Procedure* MatchingMethodWithoutArguments = NULL;
        GenericBinding* GenericBindingContextWithoutArguments = GenericBindingContext;
        MethodConversionClass MethodConversionWithoutArguments = MethodConversion;
        bool requiresNarrowingConversionWithoutArguments = pRequiresNarrowingConversion ? *pRequiresNarrowingConversion : false;
        bool someCandidatesBadWithoutArguments = false;
        bool wouldHaveSucceededWithStrictOffWithoutArguments = false;

        //Attempt with zero argument relaxation
        MatchingMethodWithoutArguments = ResolveCandidateInstanceMethodForDelegateInvokeAndReturnBindingContext
        (
            MethodOperand,
            uFlags,
            InvokeMethod,
            DelegateType,
            OriginalDelegateTypeForErrors,
            InaccessibleMatchingMethod,
            someCandidatesBadWithArguments,
            MatchingMethodTypeArgumentLocations,
            GenericBindingContextWithoutArguments,
            SuppressMethodNameInErrorMessages,
            IgnoreReturnValueErrorsForInference,
            MethodConversionWithoutArguments,
            OverloadFlags,
            &requiresNarrowingConversionWithoutArguments,
            &wouldHaveSucceededWithStrictOffWithoutArguments,
            &couldTryZeroArgumentRelaxation,
            true // Use Argument Relaxation
        );

        if (MatchingMethodWithoutArguments != NULL && (!someCandidatesBadWithoutArguments || wouldHaveSucceededWithStrictOffWithoutArguments))
        {
            if (m_Errors)
            {
                backup_error_table.EnableMergeOnRestore(withOutArgumentsIndex);
            }
            MatchingMethod = MatchingMethodWithoutArguments;
            GenericBindingContext = GenericBindingContextWithoutArguments;
            if (pRequiresNarrowingConversion != NULL)
            {
                *pRequiresNarrowingConversion = requiresNarrowingConversionWithoutArguments;
            }
            SomeCandidatesBad = someCandidatesBadWithoutArguments;
            MethodConversion = MethodConversionWithoutArguments;
        }
        else
        {
            ReportErrorsForWithArguments = true;
        }
    }
    else
    {
        ReportErrorsForWithArguments = true;
    }

    if (ReportErrorsForWithArguments)
    {
        if (m_Errors)
        {
            backup_error_table.EnableMergeOnRestore(withArgumentsIndex);
        }
        MatchingMethod = MatchingMethodWithArguments;
        GenericBindingContext = GenericBindingContextWithArguments;
        if (pRequiresNarrowingConversion != NULL)
        {
            *pRequiresNarrowingConversion = requiresNarrowingConversionWithArguments;
        }
        SomeCandidatesBad = someCandidatesBadWithArguments;
        MethodConversion = MethodConversionWithArguments;
    }

    backup_error_table.Restore();

    return MatchingMethod;
}

BCSYM_Param*
Semantics::CopyParameterList
(
    Procedure *InvokeMethod,
    BCSYM_GenericBinding *GenericBindingContext,
    bool IsRelaxedDelegateParameterList
)
{
    BCSYM_Param *ParamHead = NULL;
    BCSYM_Param *LastParam = NULL;
    WCHAR nameBuffer[20];
    unsigned parameterIndex = 0;

    // Build the parameters for the lambda expression
    for (BCSYM_Param *CurrentParam = InvokeMethod->GetFirstParam(); CurrentParam ; CurrentParam = CurrentParam->GetNext())
    {
        // Get the type of the parameter
        Type *ParameterType = GetDataType(CurrentParam);

        // Assume it doesn't have a value when building the symbol.  When we copy it over we will set the kind to the right thing.
        BCSYM_Param *ClonedParam = m_TransientSymbolCreator.AllocParameter( CurrentParam->HasLocation(), false );
        memcpy( ClonedParam, CurrentParam, sizeof( BCSYM_Param ));

        // Clear the token so that the parameter is emitted correctly.
        Symbols::SetToken(ClonedParam, mdTokenNil);

        // The location does not get copied with a memcopy (since it's actually outside the symbol)
        if ( CurrentParam->HasLocation())
        {
            ClonedParam->SetLocation( CurrentParam->GetLocation() );
        }
        ClonedParam->SetNext(NULL); // this is a copy - don't reuse the next pointer of the original

        // Bind the generic type binding since lambda's don't support generics. The Closure code
        // will lift it appropriate and guard against shadowing.
        ParameterType = ReplaceGenericParametersWithArguments(ParameterType, GenericBindingContext, m_SymbolCreator);
        ClonedParam->SetType(ParameterType);

        // Rename the parameter to have Unique names
        if (IsRelaxedDelegateParameterList)
        {
            ::StringCchPrintf(nameBuffer, _countof(nameBuffer), L"a%d", parameterIndex);
            ClonedParam->SetName(GetCompiler()->AddString(nameBuffer));
            ClonedParam->SetIsRelaxedDelegateVariable(true);
        }

        if ( LastParam )
        {
            LastParam->SetNext(ClonedParam);
        }
        else
        {
            ParamHead = ClonedParam;
        }
        LastParam = ClonedParam;
        parameterIndex++;
    }
    return ParamHead;
}

ParseTree::ArgumentList *
Semantics::CreateArgumentList
(
    BCSYM_Param *InvokeParam,
    BCSYM_Param *TargetParam,
    bool ForceCopyInvokeArguments,
    const Location &Location
)
{
    ParserHelper PH(&m_TreeStorage, Location);

    ParseTree::ArgumentList * FirstArg = NULL;
    ParseTree::ArgumentList * LastArg = NULL;
    bool ContinueBuildingArguments = ForceCopyInvokeArguments;
    VSASSERT(!ForceCopyInvokeArguments || (TargetParam == NULL), "ForceCopyInvokeArguments assumes no targetparam.");

    while ( TRUE )
    {
        ParseTree::ArgumentList * ArgListElement = NULL;

        if ( !InvokeParam ) break; // Done building parameterList
        if (TargetParam && TargetParam->IsParamArray())
        {
            // We need to keep adding the parameters if the target has a paramarray.
            // Types have been verified by MethodSHapeMatchesDelegateInvoke
            ContinueBuildingArguments = true;
        }
        if (!TargetParam && !ContinueBuildingArguments) break; // Done building parameterList

        // Create Argument
        ParseTree::Expression * ArgExpression = PH.CreateNameExpression(1, InvokeParam->GetName());
        ArgListElement = new(m_TreeStorage) ParseTree::ArgumentList;
        ArgListElement->TextSpan = Location;
        ArgListElement->Element = new(m_TreeStorage) ParseTree::Argument;
        ArgListElement->Element->TextSpan = Location;
        ArgListElement->Element->Value = ArgExpression;

        if (FirstArg == NULL) {
            FirstArg = ArgListElement;
        }

        if (LastArg != NULL) {
            LastArg->Next = ArgListElement;
        }

        LastArg = ArgListElement;

        if (InvokeParam) InvokeParam = InvokeParam->GetNext();
        if (TargetParam) TargetParam = TargetParam->GetNext();
    }

    return FirstArg;
}

void Semantics::InsertIntoProcDescriptorArray
(
    ExtensionCallInfo * pCandidate,
    GenericBindingInfo binding,
    DynamicArray<ProcedureDescriptor> * pDescriptorArray
)
{
    ThrowIfNull(pCandidate);

    if (pDescriptorArray)
    {

        ProcedureDescriptor procDescriptor;
        procDescriptor.Proc = pCandidate->m_pProc;

        procDescriptor.Binding  = binding;

        pDescriptorArray->AddElement(procDescriptor);
    }
}

bool
Semantics::ProcessExplicitTypeArgumentsForExtensionMethod
(
    ILTree::ExtensionCallExpression * pExtensionCall,
    GenericBindingInfo & binding,
    ExtensionCallInfo * pCandidate,
    DynamicArray<ProcedureDescriptor> * pAccessibleSignatureMismatches,
    Location * & pTypeArgumentLocations
)
{
    ThrowIfNull(pCandidate);
    ThrowIfNull(pExtensionCall);
    ThrowIfFalse(pExtensionCall->TypeArgumentCount);

    Assume(!pCandidate->m_pProc->IsGeneric() || ! binding.IsNull(), L"Why does a generic extension method candidate not have a partial generic binding.");

    if (binding.FreeTypeArgumentCount() == pExtensionCall->TypeArgumentCount)
    {

        binding.ApplyExplicitArgumentsToPartialBinding
        (
            pExtensionCall->TypeArguments,
            pExtensionCall->TypeArgumentLocations,
            pExtensionCall->TypeArgumentCount,
            this,
            pCandidate->m_pProc
        );

        pTypeArgumentLocations = binding.GetTypeArgumentLocations();

        return true;
    }
    else
    {
        InsertIntoProcDescriptorArray(pCandidate, binding, pAccessibleSignatureMismatches);
        return false;
    }
}

bool
Semantics::InferTypeArgumentsForExtensionMethodDelegate
(
    ILTree::ExtensionCallExpression * pExtensionCall,
    GenericBindingInfo & binding,
    ExtensionCallInfo * pCandidate,
    DynamicArray<ProcedureDescriptor> * pAccessibleTypeInferenceFailures,
    Location * & pTypeArgumentLocations,
    Procedure * pDelegateInvokeMethod,
    GenericBinding * pDelegateBinding,
    TypeInferenceLevel & typeInferenceLevel
)
{
    BackupValue<bool> backup_report_errors(&m_ReportErrors);
    m_ReportErrors = false;

    Type *pDelegateReturnType = ReplaceGenericParametersWithArguments(
        pDelegateInvokeMethod->PProc()->GetType(),
        pDelegateBinding,
        m_SymbolCreator);

    unsigned ParameterCount = pDelegateInvokeMethod->GetParameterCount();

    // Allocate the BoundArgumentList. Try stack first.
    ILTree::Expression *ArgumentsScratch[128];
    ILTree::Expression **BoundArguments = ArgumentsScratch;
    if (ParameterCount > 128)
    {
        typedef ILTree::Expression *ExpressionPointer;
        BoundArguments = new(m_TreeStorage) ExpressionPointer[ParameterCount];
    }

    // Initialze to NULL.
    for (unsigned i = 0; i < ParameterCount; i++)
    {
        BoundArguments[i] = NULL;
    }
    BoundArguments[0] = NULL;

   // Build up an argument list based on the parameters of the delegate's Invoke method
    unsigned ParameterIndex = 0;
    for (Parameter *CurrentParameter = pDelegateInvokeMethod->GetFirstParam();
           CurrentParameter;
           CurrentParameter = CurrentParameter->GetNext())
    {
        // Get the type of the parameter
        Type *ParameterType = GetDataType(CurrentParameter);

        if (ParameterType->IsPointerType())
        {
            ParameterType = ParameterType->PPointerType()->GetRoot();
        }

        ParameterType = ReplaceGenericParametersWithArguments(ParameterType, pDelegateBinding, m_SymbolCreator);

        ILTree::Expression *TypeReference =
            AllocateExpression(
                SX_NAME,
                ParameterType,
                pExtensionCall->Loc);

        // Build up an argument representing the parameter
        BoundArguments[ParameterIndex] =
            AllocateExpression(
                SX_ARG,
                TypeHelpers::GetVoidType(),
                TypeReference,
                pExtensionCall->Loc);

        ParameterIndex++;
    }

    bool ignored = false;
    bool typeInferenceOK =
        InferTypeArguments
        (
            pExtensionCall->Loc,
            pCandidate->m_pProc,
            BoundArguments,
            NULL,
            pDelegateReturnType,
            OvrldNoFlags,
            binding,
            pTypeArgumentLocations,
            typeInferenceLevel,
            ignored,
            true,
            false,
            true
        );


    if (!typeInferenceOK)
    {
        InsertIntoProcDescriptorArray(pCandidate, binding, pAccessibleTypeInferenceFailures);

        return false;
    }
    else
    {
        return true;
    }
}

Procedure *
Semantics::ResolveExtensionMethodForDelegateInvokeTryingFullAndRelaxedArgs
(
    ILTree::ExtensionCallExpression * MethodOperand,
    unsigned uFlags,
    Procedure * InvokeMethod,
    Type * DelegateType,
    Type* OriginalDelegateTypeForErrors,
    bool IgnoreReturnValueErrorsForInference,
    GenericBindingInfo & Binding,
    MethodConversionClass &MethodConversion,
    bool & ResultIsExtensionMethod,
    bool *pRequiresNarrowingConversion
)
{
    ExpressionListHelper listHelper(this, &m_TreeAllocator.xCopyBilNode(MethodOperand->ImplicitArgumentList)->AsExpression());
    GenericTypeBinding *DelegateBindingContext = TypeHelpers::IsGenericTypeBinding(DelegateType) ? DelegateType->PGenericTypeBinding() : NULL;
    Location AddressOfLocation = MethodOperand->Loc;


    for (Parameter *CurrentParameter = InvokeMethod->GetFirstParam(); CurrentParameter;CurrentParameter = CurrentParameter->GetNext())
    {
        // Get the type of the parameter
        Type *ParameterType = GetDataType(CurrentParameter);

        if (ParameterType->IsPointerType())
        {
            ParameterType = ParameterType->PPointerType()->GetRoot();
        }

        ParameterType = ReplaceGenericParametersWithArguments(ParameterType, DelegateBindingContext, m_SymbolCreator);

        ILTree::Expression *TypeReference =
            AllocateExpression
            (
                SX_NAME,
                ParameterType,
                AddressOfLocation
            );

        listHelper.Add
        (
            AllocateExpression
            (
                SX_ARG,
                TypeHelpers::GetVoidType(),
                TypeReference,
                AddressOfLocation
            ),
            AddressOfLocation
        );

    }

    BCSYM * InvokeMethodReturnType = NULL;

    if (!IgnoreReturnValueErrorsForInference )
    {
        InvokeMethodReturnType =
            ReplaceGenericParametersWithArguments
            (
                InvokeMethod->GetType(),
                DelegateBindingContext,
                m_SymbolCreator
            );
    }

     OverloadResolutionFlags OverloadFlags =
        OvrldIgnoreLateBound |
        OvrldDontReportSingletonErrors |
        (
            (
                InvokeMethodReturnType == NULL ||
                TypeHelpers::IsVoidType(InvokeMethodReturnType)
            ) ?
            OvrldPreferSubOverFunction :
            OvrldPreferFunctionOverSub
        ) |
        OvrldSomeCandidatesAreExtensionMethods |
        OvrldReturnUncallableSingletons |
        OvrldReportErrorsForAddressOf;

    TemporaryErrorTable backup_error_table(m_Compiler, &m_Errors);

    // !! DANGER !!  See the comments in TemporaryErrorTable.Restore. MatchArguments can potentially
    // have side-effects on the SX_ tree as well (in particular in alters function(x)x by adding the
    // lambda parameter's "As" type). Using a temporary error table is all well and good, but what
    // do we do about a temporary SX_ tree? There's no mechanism for it.
    // Fortunately that doesn't cause problems here, since if our first attempt at resolution failed
    // but the zero-argument attempt did, then it means there were no arguments in the SX_ tree that
    // we might have mutated!

    const unsigned withArgumentsIndex = 0;
    const unsigned withOutArgumentsIndex = 1;

    if (m_Errors)
    {
        backup_error_table.AddTemporaryErrorTable(new  ErrorTable(*m_Errors));
        m_Errors = backup_error_table.NewErrorTable(withArgumentsIndex);
    }

    Procedure * ResultToReturn = NULL;


    GenericBindingInfo  GenericBindingWithArguments;

    bool ResultWithArgumentsIsExtensionMethod = false;

    // Port SP1 CL 2967675 to VS10

    bool SomeCandidatesBad = false;
    bool IsBadSingleton = false;

    Procedure * ResultWithArguments =
        ResolveExtensionCallOverloading
        (
            MethodOperand,
            listHelper.Start(),
            listHelper.Count(),
            GenericBindingWithArguments,
            ExprNoFlags,
            OverloadFlags,
            AddressOfLocation,
            OriginalDelegateTypeForErrors,
            InvokeMethodReturnType,
            SomeCandidatesBad,
            ResultWithArgumentsIsExtensionMethod,
            IsBadSingleton,
            NULL // not interested in ambiguous async subs here, since we're only looking for the delegate invoke
        );

    if (!ResultWithArguments || SomeCandidatesBad || IsBadSingleton)
    {
        bool SomeCandidatesBadWithoutArguments = false;
        bool IsBadSingletonWithoutArguments = false;
        
        if (m_Errors)
        {
            backup_error_table.AddTemporaryErrorTable(new  ErrorTable(*backup_error_table.OldErrorTable()));
            m_Errors = backup_error_table.NewErrorTable(withOutArgumentsIndex);
        }

        GenericBindingInfo GenericBindingWithOutArguments;

        bool ResultWithoutArgumentsIsExtensionMethod = false;

        Procedure * ResultWithOutArguments =
            ResolveExtensionCallOverloading
            (
                MethodOperand,
                MethodOperand->ImplicitArgumentList,
                1,
                GenericBindingWithOutArguments,
                ExprNoFlags,
                OverloadFlags,
                AddressOfLocation,
                OriginalDelegateTypeForErrors,
                InvokeMethodReturnType,
                SomeCandidatesBadWithoutArguments,
                ResultWithoutArgumentsIsExtensionMethod,
                IsBadSingletonWithoutArguments,
                NULL  // not interested in ambiguous async subs here, since we're only looking for the delegate invoke
            );

        if (ResultWithOutArguments && ! SomeCandidatesBadWithoutArguments&& !IsBadSingletonWithoutArguments)
        {
            if (m_Errors)
            {
                backup_error_table.EnableMergeOnRestore(withOutArgumentsIndex);
            }
            ResultToReturn = ResultWithOutArguments;
            ResultIsExtensionMethod = ResultWithoutArgumentsIsExtensionMethod;
            Binding = GenericBindingWithOutArguments;
            SomeCandidatesBad = false;
            IsBadSingleton = false;
        }
        else
        {
            if (m_Errors)
            {
                backup_error_table.EnableMergeOnRestore(withArgumentsIndex);
            }
            ResultToReturn = ResultWithArguments;
            ResultIsExtensionMethod = ResultWithArgumentsIsExtensionMethod;
            Binding = GenericBindingWithArguments;
        }
    }
    else
    {
        ResultToReturn = ResultWithArguments;
        ResultIsExtensionMethod = ResultWithArgumentsIsExtensionMethod;
        Binding = GenericBindingWithArguments;

        if (m_Errors)
        {
            backup_error_table.EnableMergeOnRestore(withArgumentsIndex);
        }
    }

    backup_error_table.Restore();

    if (ResultToReturn && ! SomeCandidatesBad)
    {
        MethodConversion =
            ClassifyMethodConversion
            (
                ResultToReturn,
                Binding,
                InvokeMethod,
                DelegateBindingContext,
                IgnoreReturnValueErrorsForInference,
                &m_SymbolCreator,
                ResultIsExtensionMethod
            );
    }
    else
    {
        MethodConversion = MethodConversionError;
    }

    if (ResultIsExtensionMethod)
    {
        MethodConversion |= MethodConversionExtensionMethod;
    }

    SourceFile * SourceFile = HasFlag(uFlags, SXF_USE_STRICT_OF_TARGET_METHOD) ? ResultToReturn->GetSourceFile() : m_SourceFile;

    bool WouldHaveSucceededWithOptionStrictOff = false;

    // Port SP1 CL 2967675 to VS10
    //We need to run IsSupportedMethodConversion, even if IsBadSingleton is true.
    //This is because IsSupportedMethodConversion may set WouldHaveSucceededWithOptionStrictOff to true, which
    //will then cause a different error message to be reported (which is the correct behavior). However, we do need to check
    //IsBadSingleton if IsSupportedMethodConversion returns true because it may incorrectly classify zero argument relaxations as being OK
    //when option strict is turned off, even though we know that the zero-argument relaxation is invalid because it failed to bind correctly when we
    //tried it above (usually this is due to type inference failures).
    if
    (
        ResultToReturn != NULL &&
        (
            !IsSupportedMethodConversion
            (
                SourceFile ? SourceFile->GetOptionFlags() & OPTION_OptionStrict : true, // SourceFile may be NULL
                MethodConversion,
                &WouldHaveSucceededWithOptionStrictOff,
                pRequiresNarrowingConversion,
                true // Conversion is for AddressOf
            ) ||
            (
                IsBadSingleton
            )
        )
    )
    {
        if (m_ReportErrors)
        {
            StringBuffer MethodRepresentation;
            StringBuffer DelegateRepresentation;

            ResultToReturn->GetBasicRep
            (
                m_Compiler,
                ResultToReturn->GetContainingClass(),
                &MethodRepresentation,
                Binding.GetGenericBindingForErrorText(),
                NULL,
                true,
                ResultIsExtensionMethod ? TIP_ExtensionCall : TIP_Normal,
                Binding.GetFixedTypeArgumentBitVector()
            );

            DelegateType->PNamedRoot()->GetBasicRep
            (
                m_Compiler,
                m_Procedure == NULL ? NULL : m_Procedure->GetContainingClass(),
                &DelegateRepresentation,
                OriginalDelegateTypeForErrors->IsGenericBinding() ? OriginalDelegateTypeForErrors->PGenericBinding() : NULL,
                NULL,
                true
            );

            STRING * ContainerName = NULL;
            unsigned ErrorID = 0;

            if (ResultIsExtensionMethod)
            {
                ContainerName = ResultToReturn->GetContainingClass()->GetErrorName(m_Compiler);
                ErrorID = WouldHaveSucceededWithOptionStrictOff ? ERRID_DelegateBindingMismatchStrictOff3 : ERRID_DelegateBindingIncompatible3;
            }
            else
            {
                ErrorID = WouldHaveSucceededWithOptionStrictOff ? ERRID_DelegateBindingMismatchStrictOff2 : ERRID_DelegateBindingIncompatible2;
            }

            ReportSemanticError
            (
                ErrorID,
                AddressOfLocation,
                MethodRepresentation.GetString(),
                DelegateRepresentation.GetString(),
                ContainerName
            );
        }
        // Bail out since this isn't a legal target

        Binding = GenericBindingInfo();
        return NULL;
    }

   return ResultToReturn;

}

bool
Semantics::IsMagicDelegateConstructor
(
    Procedure *DelegateConstructor
)
{
    return
        DelegateConstructor->GetParameterCount() == 2 &&
        TypeHelpers::IsRootObjectType(GetDataType(DelegateConstructor->GetFirstParam())) &&
        TypeHelpers::IsFunctionPointerType(GetDataType(DelegateConstructor->GetFirstParam()->GetNext()), m_CompilerHost);
}

void
Semantics::VerifyTypeCharacterConsistency
(
    const Location &ErrorLocation,
    Type *ResultType,
    typeChars TypeCharacter
)
{
    if (TypeCharacter != chType_NONE)
    {
        if (TypeHelpers::IsArrayType(ResultType))
        {
            ResultType = TypeHelpers::GetElementType(ResultType->PArrayType());
        }

        if (TypeHelpers::IsNullableType(ResultType, m_CompilerHost))
        {
            ResultType = TypeHelpers::GetElementTypeOfNullable(ResultType, m_CompilerHost);
        }

        if (ResultType->GetVtype() != VtypeOfTypechar(TypeCharacter))
        {
            ReportSemanticError(
                ERRID_TypecharNoMatch2,
                ErrorLocation,
                WszTypeChar(TypeCharacter),
                ResultType);
        }
    }
}

// For an expression used as the base of a reference, determine the type to be
// used for checking the accessibility of the reference.

Type *
Semantics::InstanceTypeOfReference
(
    ILTree::Expression *Instance
)
{
    if (Instance == NULL ||
        (Instance->bilop == SX_SYM && HasFlag32(Instance, SXF_SYM_MYBASE)))
    {
        return ContainingClass();
    }

    return Instance->ResultType;
}

ILTree::Expression *
Semantics::ReferToSymbol
(
    const Location &ReferringLocation,
    Symbol *Referenced,
    typeChars TypeCharacter,
    ILTree::Expression *BaseReference,
    GenericBinding *GenericBindingContext,
    ExpressionFlags Flags
)
{
    ThrowIfNull(Referenced);
    Assume(Referenced->IsNamedRoot() || Referenced->IsExtensionCallLookupResult(), L"It should only be possible to refer to a named root or an extension call lookup result!");

    if (Referenced->IsBad())
    {
        return AllocateBadExpression(ReferringLocation);
    }
    else if (Referenced->IsExtensionCallLookupResult())
    {
        return
            ReferToExtensionMethod
            (
                ReferringLocation,
                Referenced->PExtensionCallLookupResult(),
                BaseReference,
                Flags,
                TypeCharacter
            );


    }

    Declaration * pNamed = Referenced->PNamedRoot();

    if (BaseReference &&
        TypeHelpers::IsReferenceType(BaseReference->ResultType) &&
        !TypeHelpers::IsGenericParameter(BaseReference->ResultType))
    {
        BaseReference = MakeRValue(BaseReference);

        if (IsBad(BaseReference))
        {
            return AllocateBadExpression(ReferringLocation);
        }
    }

    if (pNamed->IsVariable())
    {
        Variable *ReferencedVariable = pNamed->PVariable();
        Type *ResultType = GetDataType(ReferencedVariable);

        if (ResultType && TypeHelpers::IsPointerType(ResultType))
        {
            // This occurs for references to symbols for Byref arguments.
            ResultType = TypeHelpers::GetReferencedType(ResultType->PPointerType());
        }

        if (ResultType == NULL)
        {
            // VS Watson has captured crashes where ResultType is NULL, so fail gracefully in this case.
            // Need to investigate this issue for V.next (VS #292763).

            return AllocateBadExpression(ReferringLocation);
        }

        if (TypeHelpers::IsBadType(ResultType))
        {
            ReportBadType(ResultType, ReferringLocation);

            return AllocateBadExpression(ResultType,ReferringLocation);
        }

        if (GenericBindingContext)
        {
            ResultType = ReplaceGenericParametersWithArguments(ResultType, GenericBindingContext, m_SymbolCreator);
        }

        VerifyTypeCharacterConsistency(ReferringLocation, ResultType, TypeCharacter);

        if (ReferencedVariable->IsConstant() &&
            // Intellisense needs access to the constant symbol, not just the value.
            // However, if this is in a context that requires a constant value, there
            // is no choice but to use the value.
            //
            // 


            (!m_PreserveExtraSemanticInformation || HasFlag(Flags, ExprMustBeConstant)) &&

            // VSW379754: ReferToConstant() checks for circularities in const initializer expression and
            // substitutes the constant with the initializer expression. An error about circularity can
            // be produced too early. For cases like "Const Color As Color = Color.red",  Color.red
            // should mean (type)Color.red not (variable)Color.red.
            // Do not substitute the const by initializer and let the subsequent code
            // to decide if it is var or type 'Color'
            !(ReferencedVariable->PVariableWithValue()->GetExpression() &&
              ReferencedVariable->PVariableWithValue()->GetExpression()->IsEvaluating() &&
              !m_IsEvaluatingSyntheticConstantExpression &&
              StringPool::IsEqual(ResultType->PNamedRoot()->GetName(), ReferencedVariable->GetName())))
        {

            if (BaseReference && !BaseReference->NameCanBeType &&
                (
                    (m_Procedure == NULL && !Bindable::IsSynthetic(ReferencedVariable )) ||
                    (m_Procedure &&
                        !(  m_Procedure->IsSyntheticMethod() &&
                            m_Procedure->PSyntheticMethod()->GetSyntheticKind() != SYNTH_New &&
                            m_Procedure->PSyntheticMethod()->GetSyntheticKind() != SYNTH_SharedNew
                        )
                    )
                )
               )
            {
                ReportSemanticError(
                    WRNID_SharedMemberThroughInstance,
                    ReferringLocation);
            }

            return
                ReferToConstant(
                    ReferringLocation,
                    ReferencedVariable->PVariableWithValue(),
                    GenericBindingContext);
        }

        else if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                ReferringLocation);

            return AllocateBadExpression(ReferringLocation);
        }

        bool BaseReferenceIsStructureRValue = false;

        if (ReferencedVariable->IsShared() || ReferencedVariable->IsConstant())
        {
            if (HasFlag(Flags, ExprIsLHSOfObjectInitializer))
            {
                ReportSemanticError(
                    ERRID_SharedMemberAggrMemberInit1,
                    ReferringLocation,
                    ReferencedVariable->GetName());

                return AllocateBadExpression(ReferringLocation);
            }

            // UI calling into the core compiler helpers can result in m_Procedure being NULL
            if (BaseReference && !BaseReference->NameCanBeType &&
                (
                    (m_Procedure == NULL && !Bindable::IsSynthetic(ReferencedVariable )) ||
                    (m_Procedure &&
                        !(  m_Procedure->IsSyntheticMethod() &&
                            m_Procedure->PSyntheticMethod()->GetSyntheticKind() != SYNTH_New &&
                            m_Procedure->PSyntheticMethod()->GetSyntheticKind() != SYNTH_SharedNew
                        )
                    )
                )
               )
            {
                ReportSemanticError(
                    WRNID_SharedMemberThroughInstance,
                    ReferringLocation);
                //}
            }


            // The extraneous base reference is not supposed to be evaluated.
            if (!m_IsGeneratingXML)
            {
                // XMLGen needs to know about the BaseReference in order to be able to round-trip code.
                BaseReference = NULL;
            }
        }
        else if(!ReferencedVariable->IsLambdaMember()) // #22920
        {
            if (BaseReference == NULL)
            {
                if (ReferencedVariable->GetParent() &&
                    TypeHelpers::IsClassOrRecordType(ReferencedVariable->GetParent()))
                {
                    BaseReference =
                        SynthesizeMeReference(
                            ReferringLocation,
                            ReferencedVariable->GetParent(),
                            HasFlag(Flags, ExprSuppressMeSynthesis));

                    if (IsBad(BaseReference))
                    {
                        if (m_PreserveExtraSemanticInformation)
                        {
                            // Intellisense needs to see this as a good
                            // expression, for example in processing
                            // initializers for class fields.
                            BaseReference = NULL;
                        }
                        else
                        {
                            return AllocateBadExpression(ReferringLocation);
                        }
                    }
                }
            }

            if (BaseReference && TypeHelpers::IsValueTypeOrGenericParameter(BaseReference->ResultType))
            {
                VSASSERT(!TypeHelpers::IsGenericParameter(BaseReference->ResultType) ||
                         HasClassConstraint(BaseReference->ResultType->PGenericParam()),
                            "Member variable access through non-class constrained type param unexpected!!!");

                // Note that this is needed so that we can determine whether the structure
                // is not an LValue (eg: RField.m_x = 20 where foo is a readonly field of a
                // structure type). In such cases, the structure's field m_x cannot be modified.
                //
                // This does not apply to type params because for type params we do want to
                // allow setting the fields even in such scenarios because only class constrained
                // type params have fields and readonly reference typed fields' fields can be
                // modified.
                //
                BaseReferenceIsStructureRValue =
                    !TypeHelpers::IsGenericParameter(BaseReference->ResultType) &&
                    !HasFlag32(BaseReference, SXF_LVALUE) &&
                    !IsMeReference(BaseReference);

                BaseReference =
                    MakeValueTypeOrTypeParamBaseReferenceToField(
                        ReferencedVariable,
                        BaseReference,
                        GenericBindingContext,
                        false,
                        false);

                VSASSERT(!TypeHelpers::IsGenericParameter(BaseReference->ResultType),
                                "Unboxed type param base reference unexpected!!!");
            }
        }

        ILTree::SymbolReferenceExpression *Result =
            AllocateSymbolReference(
                ReferencedVariable,
                ResultType,
                BaseReference,
                ReferringLocation);

        Result->GenericBindingContext = GenericBindingContext;

        if (!(ReferencedVariable->IsReadOnly() || BaseReferenceIsStructureRValue) ||
                (m_Procedure &&
                 (m_Procedure->GetContainingClass() ==
                    ReferencedVariable->GetContainingClass()) &&
                 ((m_Procedure->IsInstanceConstructor() && !ReferencedVariable->IsShared() &&
                    IsMeReference(BaseReference)) ||
                  (m_Procedure->IsSharedConstructor() && ReferencedVariable->IsShared()))))
        {
            // ReadOnly fields accessed within the constructor are still RValues because the
            // lambda in the end will be generated as a separate procedure where the
            // ReadOnly field is an RValue
            if ( ! (m_InLambda && ReferencedVariable->IsReadOnly())  )
            {
                SetFlag32(Result, SXF_LVALUE);
            }
        }

        return Result;
    }
    else if (IsProcedure(pNamed))
    {
        if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                ReferringLocation);

            return AllocateBadExpression(ReferringLocation);
        }

        if (ViewAsProcedure(pNamed)->IsInstanceConstructor())
        {
            // The first statement in a constructor is expected to be
            // a call to another constructor. Otherwise, a direct
            // reference to a constructor is invalid.

            if (!HasFlag(Flags, ExprIsConstructorCall))
            {
                ReportSemanticError(
                    ERRID_InvalidConstructorCall,
                    ReferringLocation);

                return AllocateBadExpression(ReferringLocation);
            }
        }

        if (!HasFlag(Flags, ExprSuppressDefaultInstanceSynthesis) &&
            ViewAsProcedure(pNamed)->IsMyGenerated() &&
            m_Procedure &&
            !m_Procedure->IsShared() &&
            BCSYM::AreTypesEqual(m_Procedure->GetContainingClass(), ViewAsProcedure(pNamed)->GetType()))
        {
            ReportSemanticError(
                ERRID_CantReferToMyGroupInsideGroupType1,
                ReferringLocation,
                ViewAsProcedure(pNamed)->GetType());
        }

        // Overloading prevents checking for type character consistency here.

        ILTree::SymbolReferenceExpression *Result =
            AllocateSymbolReference(
                pNamed,
                TypeHelpers::GetVoidType(),
                BaseReference,
                ReferringLocation);

        Result->GenericBindingContext = GenericBindingContext;

        if (HasFlag(Flags, ExprSuppressMeSynthesis))
        {
            SetFlag32(Result, SXF_SYM_MAKENOBASE);
        }

        //if (HasFlag(Flags, ExprIsExplicitCallTarget))
        if ((Flags & (ExprIsExplicitCallTarget | ExprSuppressDefaultInstanceSynthesis)) == ExprIsExplicitCallTarget)
        {
            // if explicit call target but not under a synthetised default instane
            // it is the case: c(a) interpreted as "my.myproject.Forms.c(a)"

            // if we are supressing call generation it means that our caller is only interested in a reference
            // to the method symbol at that he will take care of generating the call.
            return Result;
        }

        return
            InterpretCallExpressionWithNoCopyout(
                ReferringLocation,
                Result,
                TypeCharacter,
                (ExpressionList *)NULL,
                false,
                Flags,
                NULL);
    }
    else if (pNamed->IsType())
    {
        if (HasFlag(Flags, ExprAllowTypeReference))
        {
            VerifyTypeCharacterConsistency(ReferringLocation, pNamed, TypeCharacter);

            BCSYM *ResultType = NULL;

            // 







            // For types, nested in generic types, the result type of the expressions should still
            // be a binding whose parent binding is the binding context of the containing binding,
            // treated as open binding
            //
            // For cases where Type arguments are needed, the expression result is just set to be
            // the raw class itself and is changed appropriately by the callers of this function.

            if (!TypeHelpers::IsGenericParameter(pNamed))
            {
                // This is to allow cases like A when A is an import alias that refers to a binding.
                // In these cases, the symbol's binding context will be a binding of the symbol itself.
                // Same whith nullable type names in expressions, i.e. Integer?.Equals(1,1)
                if (GenericBindingContext &&
                    GenericBindingContext->PGenericBinding()->GetGeneric() == pNamed)
                {
                    ResultType = GenericBindingContext;
                }
                else if (pNamed->IsGeneric())
                {
                    if (!(HasFlag(Flags, ExprSuppressTypeArgumentsChecking)))
                    {
                        ReportSemanticError(
                            ERRID_GenericTypeRequiresTypeArgs1,
                            ReferringLocation,
                            pNamed);

                        return AllocateBadExpression(ReferringLocation);
                    }
                }
                else if (IsGenericOrHasGenericParent(pNamed->GetParent()))
                {
                    if (GenericBindingContext)
                    {
                        ResultType =
                            m_SymbolCreator.GetGenericBinding(
                                false,
                                pNamed,
                                NULL,
                                0,
                                DeriveGenericBindingForMemberReference(
                                    GenericBindingContext,
                                    pNamed,
                                    m_SymbolCreator,
                                    m_CompilerHost));
                    }
                    // 














                }
            }

            ILTree::SymbolReferenceExpression *Result =
                AllocateSymbolReference(
                    pNamed,
                    ResultType ?
                        ResultType :
                        pNamed,
                    BaseReference,
                    ReferringLocation);

            Result->GenericBindingContext = GenericBindingContext;

            return Result;
        }
        else
        {
            ReportSemanticError(
                TypeHelpers::IsClassType(pNamed) ?
                    ERRID_ClassNotExpression1 :
                    (TypeHelpers::IsInterfaceType(pNamed) ?
                        ERRID_InterfaceNotExpression1 :
                        (TypeHelpers::IsEnumType(pNamed) ?
                            ERRID_EnumNotExpression1 :
                            (TypeHelpers::IsRecordType(pNamed) ?
                                ERRID_StructureNotExpression1 :
                                ERRID_TypeNotExpression1))),
                ReferringLocation,
                pNamed);

            return AllocateBadExpression(ReferringLocation);
        }
    }
    else if (IsNamespace(pNamed))
    {
        if (HasFlag(Flags, ExprAllowNamespaceReference))
        {
            return
                AllocateSymbolReference(
                    pNamed->PNamedRoot(),
                    TypeHelpers::GetVoidType(),
                    BaseReference,
                    ReferringLocation);
        }
        else
        {
            // Avoid a bad substitution due to a single "Global" keyword in an expression.
            if (pNamed->IsNamespace() && wcscmp(pNamed->PNamespace()->GetName(), L"") == 0)
            {
                ReportSemanticError(
                    ERRID_NamespaceNotExpression1,
                    ReferringLocation,
                    L"Global");
            }
            else
            {
                ReportSemanticError(
                    ERRID_NamespaceNotExpression1,
                    ReferringLocation,
                    pNamed);
            }

            return AllocateBadExpression(ReferringLocation);
        }
    }

    VSFAIL("Bridge not there.");
    return NULL;
}

ConstantValue
Semantics::GetConstantValue
(
    const Location &ReferringLocation,
    SymbolicValue *Value
)
{
    ConstantValue Result;

    if (Value == NULL)
    {
        // An absent value is a bad value.
        Result.TypeCode = t_bad;
        return Result;
    }

    // Report a circular constant dependency error if the expression
    // is a user defined expression (non-synthetic expression), else
    // continue evaluating as though there is no cycle and eventually
    // we will reach a user defined constant expression that is involved
    // in the cycle on which we can report the error.
    //
    // There is a valid assumption made here:
    //  - Synthetic constant expressions cannot have cycles among
    //    themselves i.e. there is always an explicit user defined
    //    constant expression involved in constant cycles.
    //
    else if (Value->IsEvaluating() &&
             !m_IsEvaluatingSyntheticConstantExpression)
    {
        ReportSemanticError(
            ERRID_CircularEvaluation1,
            ReferringLocation,
            Value->GetReferringDeclaration());

        Result.TypeCode = t_bad;
        return Result;
    }

    if (!Value->IsEvaluated())
    {
        Container *ContainerContainingConstant =
            Value->GetReferringDeclaration()->GetContainer();

        VSASSERT(!ContainerContainingConstant->IsBindingDone(),
                    "How can a constant expression's container be bound completely without the expression being evaluated ?");

        Bindable::EvaluateDeclaredExpression(
            Value,
            &m_TreeStorage,
            // Find the allocator associated with the source file containing the
            // referenced symbol, so that storage allocated for the symbol's
            // constant value has the same lifetime as the symbol.
            //
            ContainerContainingConstant->GetBindableInstance()->CurrentAllocator(),
            // Use the error table of the current compiler step associated with the symbol's container
            //
            ContainerContainingConstant->GetBindableInstance()->
                CurrentErrorLog(Value->GetReferringDeclaration()),
            m_Compiler,
            m_CompilerHost,
            m_CompilationCaches);
    }

    if (Value->IsBadExpression())
    {
        Result.TypeCode = t_bad;
        return Result;
    }

    return Value->GetValue();
}

ILTree::Expression *
Semantics::ReferToConstant
(
    const Location &ReferringLocation,
    Constant *Referenced,
    GenericBinding *GenericBindingContext
)
{
    ConstantValue ResultValue = GetConstantValue(ReferringLocation, Referenced->GetExpression());

    if (ResultValue.TypeCode == t_bad)
    {
        return AllocateBadExpression(ReferringLocation);
    }

    Referenced->SetIsUsed();  // hack to make sure the constant is marked as used

    Type *ResultType = GetDataType(Referenced);

    if (GenericBindingContext)
    {
        ResultType = ReplaceGenericParametersWithArguments(ResultType, GenericBindingContext, m_SymbolCreator);
    }

    // If the constant variable was not declared with an explicit type
    // use the type of the constant expression as the result type.

    if (TypeHelpers::IsUndefinedType(ResultType))
    {
        ResultType = GetFXSymbolProvider()->GetType(ResultValue.TypeCode);
    }

    ILTree::Expression *Result =
        ProduceConstantExpression(ResultValue, ReferringLocation, ResultType IDE_ARG(0));

    IDE_CODE(Result->uFlags |= SXF_CON_CONTAINS_NAMED_CONTANTS);

    return Result;
}

ILTree::Expression *
Semantics::EnclosingWithValue
(
    const Location &ReferringLocation,
    ExpressionFlags Flags /* = ExprNoFlags */
)
{
    if (m_EnclosingWith == NULL)
    {
        // DevDiv Bugs #28433
        if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_BadWithRefInConstExpr,
                ReferringLocation);
        }
        else
        {
            ReportSemanticError(ERRID_BadWithRef, ReferringLocation);
        }

        return AllocateBadExpression(ReferringLocation);
    }

    if (IsBad(m_EnclosingWith))
    {
        return AllocateBadExpression(ReferringLocation);
    }

    ILTree::Expression *Result = NULL;

    if (m_EnclosingWith->ObjectBeingInitialized)
    {
        // Clone the "With" context expression so that the same copy is
        // not used to represent multiple nodes in the bound "TREE".
        Result = m_TreeAllocator.xCopySymbolReferenceTree(m_EnclosingWith->ObjectBeingInitialized);
    }
    else if (HasFlag32(m_EnclosingWith, SBF_WITH_RECORD))
    {
        // A With with a Record type used a clonable Lvalue instead of a
        // captured Rvalue.

        ILTree::Expression *WithReference = NULL;
        UseTwiceShortLived(m_EnclosingWith->RecordReference, WithReference, Result);

        VSASSERT(
            WithReference == m_EnclosingWith->RecordReference,
            "Allegedly side-effect-free tree mutated during cloning");

        Result->Loc = ReferringLocation;
    }
    else
    {
        Declaration *WithTemporary =
            m_EnclosingWith->TemporaryBindAssignment->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol;

        // The type of the With temporary does not necessarily match
        // the type of the With value. In particular, if the With value
        // is any class type, the type of the With temporary is Object.
        // Fetch the type of the With value.

        Result =
            AllocateSymbolReference(
                WithTemporary,
                m_EnclosingWith->TemporaryBindAssignment->AsExpressionWithChildren().Right->ResultType,
                NULL,
                ReferringLocation);
    }

    // Don't rely on the current state of the LValue flag. Make sure that
    // it is correct.

    if (HasFlag32(m_EnclosingWith, SBF_WITH_LVALUE))
    {
        SetFlag32(Result, SXF_LVALUE);
    }
    else
    {
        ClearFlag32(Result, SXF_LVALUE);
    }

    return Result;
}

static Declaration *
TargetProcedureForErrorMessage
(
    Procedure *TargetProcedure,
    Declaration *RepresentTargetInMessages
)
{
    if (RepresentTargetInMessages)
    {
        return RepresentTargetInMessages;
    }

    return TargetProcedure;
}

void
Semantics::EnforceArgumentNarrowing
(
    ILTree::Expression *Argument,
    Type *OriginalArgumentType,
    ILTree::Expression *OriginalArgument,
    Parameter *Param,
    Type *TargetType,
    bool RejectNarrowingConversions,
    bool NarrowingIsInCopyBack,
    bool NarrowingFromNumericLiteral,
    bool &SomeArgumentsBad,
    bool &RequiresNarrowingConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral
)
{
    if (RejectNarrowingConversions)
    {
        ILTree::Expression * Lambda = Argument;
        Type * GenericExpressionType = NULL;

        // 


        if(!NarrowingIsInCopyBack &&
            OriginalArgument && OriginalArgument->bilop == SX_LAMBDA &&
            (OriginalArgumentType == TypeHelpers::GetVoidType() || OriginalArgumentType->IsAnonymousDelegate()) && // We would try to covert
            (Lambda->bilop == SX_LAMBDA ||
                (Lambda->bilop == SX_WIDE_COERCE && Lambda->ResultType &&
                  TypeHelpers::IsGenericTypeBinding(Lambda->ResultType) &&
                  (GenericExpressionType = GetFXSymbolProvider()->GetGenericExpressionType()) &&
                  TypeHelpers::EquivalentTypes(Lambda->ResultType->PGenericTypeBinding()->GetGenericType(),GenericExpressionType)  &&
                 (Lambda=Lambda->AsBinaryExpression().Left) && Lambda->bilop == SX_LAMBDA)))
        {
            ReportSemanticError(
                ERRID_NestedFunctionArgumentNarrowing3,
                Argument->Loc,
                Param->GetName(),
                OriginalArgument->AsLambdaExpression().GetExpressionLambdaBody()->ResultType,
                Lambda->AsLambdaExpression().GetExpressionLambdaBody()->ResultType);
        }
        else if(!NarrowingIsInCopyBack &&
                    OriginalArgumentType == TypeHelpers::GetVoidType())
        {
            ReportSemanticError(
                ERRID_ArgumentNarrowing2,
                Argument->Loc,
                Param->GetName(),
                TargetType);
        }
        else
        {
            ReportSemanticError(
                NarrowingIsInCopyBack ?
                    ERRID_ArgumentCopyBackNarrowing3 :
                    ERRID_ArgumentNarrowing3,
                Argument->Loc,
                Param->GetName(),
                OriginalArgumentType,
                TargetType);
        }

        RequiresNarrowingConversion = false;
        SomeArgumentsBad = true;
    }
    else
    {
        RequiresNarrowingConversion = true;

        if (!TypeHelpers::IsRootObjectType(OriginalArgumentType))
        {
            AllNarrowingIsFromObject = false;
        }

        if (!NarrowingFromNumericLiteral)
        {
            AllNarrowingIsFromNumericLiteral = false;
        }
    }
}

// This is similar to the old MakeRValue, but it does extra work to carry out information about narrowings-during-reclassification. 
// (Actually, for historical reasons, it continues to leave Lambdas/AddressOfs alone. They aren't reclassified, and we don't carry 
// out their reclassification-narrowings.)
ILTree::Expression *
Semantics::MakeRValueArgument
(
    ILTree::Expression *Argument,
    BCSYM *TargetType,
    bool &RequiresNarrowingConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral
)
{
    // Dev10 #846708 Explicitly reclassify array literals to get information about narrowing, etc.
    if (!IsBad(Argument) && Argument->ResultType->IsArrayLiteralType() && Argument->bilop==SX_ARRAYLITERAL)
    {
        bool _RequiresNarrowingConversion = false;
        bool _NarrowingFromNumericLiteral = false;

        ILTree::Expression * pResult = ConvertArrayLiteral(&Argument->AsArrayLiteralExpression(), TargetType,
                        _RequiresNarrowingConversion,
                        _NarrowingFromNumericLiteral
                    );

        if (pResult != NULL)
        {
            if (_RequiresNarrowingConversion)
            {
                RequiresNarrowingConversion = true;

                if (!_NarrowingFromNumericLiteral)
                {
                    AllNarrowingIsFromNumericLiteral = false;
                }

                AllNarrowingIsFromObject = false;
            }

            Argument = pResult;

            AssertIfFalse (IsBad(Argument) || TypeHelpers::EquivalentTypes(TargetType, Argument->ResultType));
        }

        // If conversion failed, MakeRValue call will try to convert again and will cause the same errors to be reported again.
        // That is why we clear LValue flag manually.
        ClearFlag32(Argument, SXF_LVALUE);
    }
    else
    {
        Argument = MakeRValue(Argument, TargetType);
        // Microsoft 2008.Aug.29. Dev10#487876: actually, that's a bad way of looking at it. Consider passing
        // argument "AddressOf Main" to a parameter type "Action()". If we tried to turn
        // AddressOf Main into an RValue then it'd fail. The above code takes advantage
        // of the fact that MakeRValue doesn't actually MakeRValue when given AddressOf/Lambda/Nothing.
        // The best solution is to redo the whole sorry MakeRValue code into something that follows
        // the reclassification rules of the spec. But until then, we have to live with it.
    }

    return Argument;
}


ILTree::Expression *
Semantics::PassArgumentByval
(
    ILTree::Expression *Argument,
    Parameter *Param,
    Type *TargetType,
    ExpressionFlags CallFlags,
    bool CheckValidityOnly,
    bool RejectNarrowingConversions,
    bool &SomeArgumentsBad,
    bool &RequiresNarrowingConversion,
    bool &RequiresSomeConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral,
    bool SuppressMethodNameInErrorMessages,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    bool & RequiresUnwrappingNullable,
    bool & RequireInstanceMethodBinding,
    AsyncSubAmbiguityFlags *pAsyncSubArgumentAmbiguity
)
{

    // The division of responsibility is a little confusing here.
    // (1) Things like SX_UNBOUND_LAMBDAS have already been resolved into SX_LAMBDAs by the time we get here
    //     through type inference with the target type. Likewise SX_ADDRESSOF
    // (2) Things like SX_ARRAYLITERALs are still present. These have to be resolved into SX_ARRAYs through
    //     type inference with the target type, but we do so here in this function.
    // (3) Identifiers and properties and other lvalues have to be resolved into rvalues.

    if (CheckValidityOnly && Argument->bilop == SX_LATE_REFERENCE)
    {
        Argument =
            AllocateExpression(
                SX_BOGUS,
                Argument->ResultType,
                Argument->Loc);
    }
    else
    {
        Type *ArgumentType = Argument->ResultType;

        Argument = MakeRValueArgument(Argument, TargetType,
                                                RequiresNarrowingConversion,
                                                AllNarrowingIsFromObject,
                                                AllNarrowingIsFromNumericLiteral
                                            );

        if (!IsBad(Argument)
            && ArgumentType->IsArrayLiteralType()
            && IsAppliedAttributeContext()
            && TypeHelpers::EquivalentTypes(TargetType, Argument->ResultType)
            && !IsValidAttributeConstant(Argument))
        {
            // Check validity of the inferred type for an array literal. It needs to be tested here
            // because it won't be caught later by ConvertWithErrorChecking.
            ReportSemanticError(
                ERRID_RequiredAttributeConstConversion2,
                Argument->Loc,
                ArgumentType,
                TargetType);

            Argument = MakeBad(Argument);
        }
    }

    if (IsBad(Argument))
    {
        //The argument is bad, thus making the method not callable.
        //We set SomeArgumentsBad to indicate this.
        //We also set RequireInstanceMethodBinding to false to give
        //extension mehtods the chance to bind.
        //This will allow extension methods to appear inside overload resolution
        //error messages.
        SomeArgumentsBad = true;
        RequireInstanceMethodBinding = false;
        return Argument;
    }

    if (TypeHelpers::EquivalentTypes(TargetType, Argument->ResultType))
    {
        return Argument;
    }

    RequiresSomeConversion = true;

    Type *OriginalArgumentType = Argument->ResultType;
    ILTree::Expression *OriginalArgument = Argument;
    bool ArgumentRequiresNarrowingConversion = false;
    bool ArgumentNarrowingFromNumericLiteral = false;
    bool ArgumentRequiresUnwrappingNullable = false;

    Argument =
        ConvertWithErrorChecking
        (
            Argument,
            TargetType,
            (HasFlag(CallFlags, ExprArgumentsMustBeConstant) ?
                ExprMustBeConstant :
                ExprNoFlags) |
            ExprGetLambdaReturnTypeFromDelegate,
            NULL,
            ArgumentRequiresNarrowingConversion,
            ArgumentNarrowingFromNumericLiteral,
            SuppressMethodNameInErrorMessages,
            DelegateRelaxationLevel,
            ArgumentRequiresUnwrappingNullable,
            pAsyncSubArgumentAmbiguity
        );

    if (IsBad(Argument))
    {
        //ConvertWithErrorChecking was unable to convert
        //the argument to the parameter type.
        //This makes the method not callable.
        //We indicate this by setting SomeArgumentsBad to true.
        //We also set RequireInstanceMethodBinding to false to allow
        //extension methods that have parameter types that can be converted to
        //the oportunity to bind.
        SomeArgumentsBad = true;
        RequireInstanceMethodBinding = false;
    }

    else if (ArgumentRequiresNarrowingConversion)
    {
        //Although EnforceArgumentNarrowing may affect the callability of the method by setting
        //SomeArgumentsBad to true we do not need to pass in
        //RequireInstanceMethodBinding and set it to false in that case. This is because under any circumstances
        //where EnforceArgumentNarrowing sets SomeArgumentBad to true
        //it will also set RequiresNarrowingConversion to true.
        //If an instance method call RequiresNarrowingConversion to true then it implictly allows extension
        //methods to bind (in order to prevent binding to extension methods there must be at least one
        //instance method that is widening and either callable or marked as RequiringInstanceMethodBinding). In this
        //case because RequiresNarrowingConversion will be true the calue of RequireInstanceMethodBinding is unimportant.
        EnforceArgumentNarrowing(
            Argument,
            OriginalArgumentType,
            OriginalArgument,
            Param,
            TargetType,
            RejectNarrowingConversions,
            false,
            ArgumentNarrowingFromNumericLiteral,
            SomeArgumentsBad,
            RequiresNarrowingConversion,
            AllNarrowingIsFromObject,
            AllNarrowingIsFromNumericLiteral);

        RequiresUnwrappingNullable = RequiresUnwrappingNullable || ArgumentRequiresUnwrappingNullable;
    }

    return Argument;
}

bool
Semantics::CanPassToParamArray
(
    ILTree::Expression *Argument,
    Type *ParamArrayType
)
{
    if (IsNothingLiteral(Argument))
    {
        return true;
    }

    Procedure *OperatorMethod = NULL;
    GenericBinding *OperatorMethodGenericContext = NULL;
    bool OperatorMethodIsLifted;

    ConversionClass ArrayConversionClassification =
        ClassifyConversion(
            ParamArrayType,
            Argument->ResultType,
            OperatorMethod,
            OperatorMethodGenericContext,
            OperatorMethodIsLifted);

    return
        ArrayConversionClassification == ConversionIdentity ||
        ArrayConversionClassification == ConversionWidening;
}

ExpressionList *
Semantics::MatchArguments3
(
    const Location &CallLocation,
    Procedure *TargetProcedure,
    Declaration *RepresentTargetInMessages,
    GenericBindingInfo  & GenericBindingContext,
    ExpressionList *Arguments,
    Type *DelegateReturnType,
    ExpressionFlags CallFlags,
    OverloadResolutionFlags OvrldFlags,
    ExpressionList *&CopyOutArguments,
    bool CheckValidityOnly,
    bool RejectNarrowingConversions,
    bool DisallowParamArrayExpansion,
    bool DisallowParamArrayExactMatch,
    bool &SomeArgumentsBad,
    bool &ArgumentArityBad,
    bool &RequiresNarrowingConversion,
    bool &RequiresSomeConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral,
    bool &InferenceFailed,
    bool &AllFailedInferenceIsDueToObject,
    bool SuppressMethodNameInErrorMessages,
    bool CandidateIsExtensionMethod,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    TypeInferenceLevel &TypeInferenceLevel,
    bool & RequiresUnwrappingNullable,
    bool & RequiresInstanceMethodBinding
)
{
    ThrowIfNull(TargetProcedure);

    IBitVector * pBitVector = GenericBindingContext.GetFixedTypeArgumentBitVector();
    bool needToDeallocateBitVector = false;

    if (CandidateIsExtensionMethod && !pBitVector)
    {
        pBitVector = new (zeromemory) BitVector<>();

        TargetProcedure->GenerateFixedArgumentBitVectorFromFirstParameter(pBitVector, m_Compiler);
        needToDeallocateBitVector = true;
    }

    bool UsedDefaultForAnOptionalParameter = false;

    ExpressionList * pRet = MatchArguments4
    (
        CallLocation,
        TargetProcedure,
        RepresentTargetInMessages,
        GenericBindingContext,
        Arguments,
        DelegateReturnType,
        CallFlags,
        OvrldFlags,
        CopyOutArguments,
        CheckValidityOnly,
        RejectNarrowingConversions,
        DisallowParamArrayExpansion,
        DisallowParamArrayExactMatch,
        SomeArgumentsBad,
        ArgumentArityBad,
        RequiresNarrowingConversion,
        RequiresSomeConversion,
        AllNarrowingIsFromObject,
        AllNarrowingIsFromNumericLiteral,
        InferenceFailed,
        AllFailedInferenceIsDueToObject,
        SuppressMethodNameInErrorMessages,
        CandidateIsExtensionMethod,
        pBitVector,
        DelegateRelaxationLevel,
        TypeInferenceLevel,
        RequiresUnwrappingNullable,
        RequiresInstanceMethodBinding,
        UsedDefaultForAnOptionalParameter
    );

    if (pBitVector && needToDeallocateBitVector)
    {
        delete pBitVector;
    }

    return pRet;
}


void
Semantics::DetectArgumentArityErrors
(
    const Location &CallLocation,
    Procedure *TargetProcedure,
    GenericBindingInfo GenericBindingContext,
    ExpressionList *Arguments,
    Type *DelegateReturnType,
    bool SuppressMethodNameInErrorMessages,
    bool CandidateIsExtensionMethod,
    OverloadResolutionFlags OvrldFlags,
    ExpressionFlags CallFlags,
    bool &ArgumentArityBad
)
{
    VSASSERT(HasFlag(CallFlags, ExprCreateColInitElement), "please only call DetectArgumentArityErrors for collection initializers");
    //
    // This function checks that the given argument list has a correct arity for the target procedure,
    // i.e. it supplies neither too many arguments nor too few. (In general the target procedure might
    // have paramarrays and optional arguments, so we don't attempt the calculation ourselves;
    // we just fob it off onto MatchArguments).
    //
    // If we find errors, then we report them.
    // 




    bool SomeArgumentsBad = false;
    bool RequiresNarrowingConversion = false;
    bool RequiresSomeConversion = false;
    bool AllNarrowingIsFromObject = true;
    bool AllNarrowingIsFromNumericLiteral = true;
    bool InferenceFailed = false;
    bool AllFailedInferenceIsDueToObject = true;
    DelegateRelaxationLevel DelegateRelaxationLevel = DelegateRelaxationLevelNone;
    TypeInferenceLevel TypeInferenceLevel = TypeInferenceLevelNone;
    ExpressionList *CopyOutArguments = NULL;
    bool RequiresUnwrappingNullable = false;
    bool RequiresInstanceMethodBinding = false;
    ArgumentArityBad = false;


    // What's important is that we want to be pure functional code, i.e. no side-effects.
    // So: suppress changes to the error table:
    //
    BackupValue<bool> backup_report_errors(&m_ReportErrors);
    m_ReportErrors = false;

    // Also: well, we can't help that MatchArguments3 is built so it modifies the Argument strucutre
    // you passed to it. To work around that, we have to make a temporary copy of the Argument
    // structure, and we'll copy it back afterwards:
    //
    ExpressionListHelper listHelper(this, Arguments);
    unsigned long cArguments = listHelper.Count();
    ILTree::Expression *ArgumentsScratch[128], **SavedArguments;
    SaveArguments(m_TreeStorage, ArgumentsScratch, 128, SavedArguments, Arguments, listHelper.Count());
    MakeScratchCopiesOfArguments(m_TreeAllocator, SavedArguments, Arguments);

    MatchArguments3
    (
        CallLocation,
        TargetProcedure,
        NULL,
        GenericBindingContext,
        Arguments,
        DelegateReturnType,
        CallFlags,
        OvrldFlags,
        CopyOutArguments,
        false,
        false,
        false,
        false,
        SomeArgumentsBad,
        ArgumentArityBad,
        RequiresNarrowingConversion,
        RequiresSomeConversion,
        AllNarrowingIsFromObject,
        AllNarrowingIsFromNumericLiteral,
        InferenceFailed,
        AllFailedInferenceIsDueToObject,
        SuppressMethodNameInErrorMessages,
        CandidateIsExtensionMethod,
        DelegateRelaxationLevel,
        TypeInferenceLevel,
        RequiresUnwrappingNullable,
        RequiresInstanceMethodBinding
    );

    RestoreOriginalArguments(SavedArguments, Arguments);
    backup_report_errors.Restore();

    if (!ArgumentArityBad)
    {
        return;
    }

    // We're going to report an error message of the form "No Add takes 1 arguments. Expected '{key,value}'."
    // So here we construct the "key,value" string:
    StringBuffer sb1,sb2;
    for (BCSYM_Param *param = TargetProcedure->GetFirstParam(); param!=NULL; param=param->GetNext())
    {
        if (param!=TargetProcedure->GetFirstParam())
        {
            sb1.AppendString(L",");
        }
        sb1.AppendSTRING(param->GetName());
    }
    // And here is the number of arguments:
    WCHAR ArgumentCountString[20];
    if (FAILED(StringCchPrintf(ArgumentCountString, DIM(ArgumentCountString), L"%lu", cArguments)))
    {
            ArgumentCountString[0] = UCH_NULL;
    }
    
     
    // "No overload for method 'Add' takes |1 arguments. Expected '{|2}'."
    ReportSemanticError
    (
        ERRID_CollectionInitializerArity2,
        CallLocation,
        ArgumentCountString,
        sb1.GetString()
    );
}



ExpressionList *
Semantics::MatchArguments1
(
    const Location &CallLocation,
    Procedure *TargetProcedure,
    Declaration *RepresentTargetInMessages,
    GenericBinding * & GenericBindingContext,
    ExpressionList *Arguments,
    Type *DelegateReturnType,
    ExpressionFlags CallFlags,
    OverloadResolutionFlags OvrldFlags,
    ExpressionList *&CopyOutArguments,
    bool CheckValidityOnly,
    bool RejectNarrowingConversions,
    bool DisallowParamArrayExpansion,
    bool DisallowParamArrayExactMatch,
    bool &SomeArgumentsBad,
    bool &ArgumentArityBad,
    bool &RequiresNarrowingConversion,
    bool &RequiresSomeConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral,
    bool &InferenceFailed,
    bool &AllFailedInferenceIsDueToObject,
    bool SuppressMethodNameInErrorMessages,
    bool CandidateIsExtensionMethod,
    IReadonlyBitVector * FixedTypeArgumentBitVector,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    TypeInferenceLevel &TypeInferenceLevel,
    bool & RequiresUnwrappingNullable,
    bool & RequiresInstanceMethodBinding,
    Location *pCallerInfoLineNumber
)
{

    if (HasFlag(CallFlags, ExprCreateColInitElement))
    {
        // For collection initializers, if you supply too many or too few arguments
        //    new Dictionary(Of String, Integer) from {{1}}
        // it tries Dictionary.Add(1) which, unless we're careful, would give confusing error messages.
        // In this case MatchArguments would go through the arguments in order and report a narrowing error on "1".
        // Then it'd discover that there aren't enough arguments and would report a NotEnoughArguments error.
        //
        // But what we want instead is to give a more straightforward "arity" error if the user got the arity
        // wrong. This has to be done in a separate sweep before MatchArguments. (that's because MatchArguments
        // would report the narrowing error on "1" before it even discovered the arity problems).
        // So here's our beforehand test for arity errors:

        bool someArityErrors=false;
        DetectArgumentArityErrors(
            CallLocation, TargetProcedure, GenericBindingContext, Arguments, DelegateReturnType, SuppressMethodNameInErrorMessages,
            CandidateIsExtensionMethod, OvrldFlags, CallFlags, someArityErrors);

        if (someArityErrors)
        {
            return MakeBad(Arguments);
            // If there was an arity error, then we won't even attempt to match the arguments, so that e.g.
            // in the above case it doesn't even report the narrowing error on "1".
        }
    }


    GenericBindingInfo binding = GenericBindingContext;
    bool UsedDefaultForAnOptionalParameter = false;

    ExpressionList * ret =
        MatchArguments4
        (
            CallLocation,
            TargetProcedure,
            RepresentTargetInMessages,
            binding,
            Arguments,
            DelegateReturnType,
            CallFlags,
            OvrldFlags,
            CopyOutArguments,
            CheckValidityOnly,
            RejectNarrowingConversions,
            DisallowParamArrayExpansion,
            DisallowParamArrayExactMatch,
            SomeArgumentsBad,
            ArgumentArityBad,
            RequiresNarrowingConversion,
            RequiresSomeConversion,
            AllNarrowingIsFromObject,
            AllNarrowingIsFromNumericLiteral,
            InferenceFailed,
            AllFailedInferenceIsDueToObject,
            SuppressMethodNameInErrorMessages,
            CandidateIsExtensionMethod,
            FixedTypeArgumentBitVector,
            DelegateRelaxationLevel,
            TypeInferenceLevel,
            RequiresUnwrappingNullable,
            RequiresInstanceMethodBinding,
            UsedDefaultForAnOptionalParameter,
            NULL,
            pCallerInfoLineNumber
        );

    GenericBindingContext = binding.PGenericBinding(false);

    return ret;
}

ExpressionList *
Semantics::MatchArguments2
(
    const Location &CallLocation,
    Procedure *TargetProcedure,
    Declaration *RepresentTargetInMessages,
    GenericBinding * & GenericBindingContext,
    ExpressionList *Arguments,
    Type *DelegateReturnType,
    ExpressionFlags CallFlags,
    OverloadResolutionFlags OvrldFlags,
    ExpressionList *&CopyOutArguments,
    bool CheckValidityOnly,
    bool RejectNarrowingConversions,
    bool DisallowParamArrayExpansion,
    bool DisallowParamArrayExactMatch,
    bool &SomeArgumentsBad,
    bool &ArgumentArityBad,
    bool &RequiresNarrowingConversion,
    bool &RequiresSomeConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral,
    bool &InferenceFailed,
    bool &AllFailedInferenceIsDueToObject,
    bool SuppressMethodNameInErrorMessages,
    bool CandidateIsExtensionMethod,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    TypeInferenceLevel &TypeInferenceLevel,
    bool & RequiresUnwrappingNullable,
    bool & RequiresInstanceMethodBinding
)
{
    ThrowIfNull(TargetProcedure);

    IBitVector * pBitVector = NULL;
    if (CandidateIsExtensionMethod)
    {
        pBitVector = new (zeromemory) BitVector<>();
        TargetProcedure->GenerateFixedArgumentBitVectorFromFirstParameter(pBitVector, m_Compiler);
    }

    ExpressionList * pRet = MatchArguments1
    (
        CallLocation,
        TargetProcedure,
        RepresentTargetInMessages,
        GenericBindingContext,
        Arguments,
        DelegateReturnType,
        CallFlags,
        OvrldFlags,
        CopyOutArguments,
        CheckValidityOnly,
        RejectNarrowingConversions,
        DisallowParamArrayExpansion,
        DisallowParamArrayExactMatch,
        SomeArgumentsBad,
        ArgumentArityBad,
        RequiresNarrowingConversion,
        RequiresSomeConversion,
        AllNarrowingIsFromObject,
        AllNarrowingIsFromNumericLiteral,
        InferenceFailed,
        AllFailedInferenceIsDueToObject,
        SuppressMethodNameInErrorMessages,
        CandidateIsExtensionMethod,
        pBitVector,
        DelegateRelaxationLevel,
        TypeInferenceLevel,
        RequiresUnwrappingNullable,
        RequiresInstanceMethodBinding
    );

    if (pBitVector)
    {
        delete pBitVector;
    }

    return pRet;
}


void
Semantics::ReportMethodCallError
(
    bool SuppressMethodNameInErrorMessages,
    bool CandidateIsExtensionMethod,
    bool ErrorIsForDelegateBinding,
    RESID SuppressMethodNameErrorID,
    RESID RegularMethodCallErrorID,
    RESID ExtensionCallErrorID,
    RESID DelegateBindingErrorID,
    const Location & Location,
    _In_opt_z_ STRING * Substitution1,
    Declaration * TargetToUseForSubstitutions2And3,
    IReadonlyBitVector * FixedTypeArgumentBitVector,
    GenericBindingInfo GenericBindingContext
)
{
    ThrowIfNull(TargetToUseForSubstitutions2And3);

    if (!m_ReportErrors)
    {
        // Don't report error if this flag is not set. ReportSemanticError handles this,
        // but ExtractErrorName does not.
        return;
    }

    RESID errorID = 0;

    if (ErrorIsForDelegateBinding)
    {
        errorID = DelegateBindingErrorID;
    }
    else if (SuppressMethodNameInErrorMessages)
    {
        errorID = SuppressMethodNameErrorID;
    }
    else if (CandidateIsExtensionMethod)
    {
        errorID = ExtensionCallErrorID;
    }
    else
    {
        errorID = RegularMethodCallErrorID;
    }

    StringBuffer textBuffer;

    ReportSemanticError
    (
        errorID,
        Location,
        Substitution1,
        ExtractErrorName
        (
            TargetToUseForSubstitutions2And3,
            textBuffer,
            CandidateIsExtensionMethod,
            FixedTypeArgumentBitVector,
            GenericBindingContext.GetGenericBindingForErrorText()
        ),
        TargetToUseForSubstitutions2And3->GetContainer()->GetQualifiedName()
    );
}


ExpressionList *
Semantics::MatchArguments4
(
    const Location &CallLocation,
    Procedure *TargetProcedure,
    Declaration *RepresentTargetInMessages,
    GenericBindingInfo & GenericBindingContext,
    ExpressionList *Arguments,
    Type *DelegateReturnType,
    ExpressionFlags CallFlags,
    OverloadResolutionFlags OvrldFlags,
    ExpressionList *&CopyOutArguments,
    bool CheckValidityOnly,
    bool RejectNarrowingConversions,
    bool DisallowParamArrayExpansion,
    bool DisallowParamArrayExactMatch,
    bool &SomeArgumentsBad,
    bool &ArgumentArityBad,
    bool &RequiresNarrowingConversion,
    bool &RequiresSomeConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral,
    bool &InferenceFailed,
    bool &AllFailedInferenceIsDueToObject,
    bool SuppressMethodNameInErrorMessages,
    bool CandidateIsExtensionMethod,
    IReadonlyBitVector * FixedTypeArgumentBitVector,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    TypeInferenceLevel &TypeInferenceLevel,
    bool & RequiresUnwrappingNullable,
    bool & RequireInstanceMethodBinding,
    bool & UsedDefaultForAnOptionalParameter,
    AsyncSubAmbiguityFlagCollection **ppAsyncSubArgumentListAmbiguity,
    Location *pCallerInfoLineNumber
)
{
    unsigned ParameterCount = TargetProcedure->GetParameterCount();
    bool TargetIsDllDeclare = TargetProcedure->IsDllDeclare();

    // Create a array of Expressions to represent matchings of arguments to
    // parameters. Index positions in the array correspond to positions in
    // the procedure's parameter list.

    ILTree::Expression *ArgumentsScratch[128];
    ILTree::Expression **BoundArguments = ArgumentsScratch;
    if (ParameterCount > 128)
    {
        typedef ILTree::Expression *ExpressionPointer;
        BoundArguments = new(m_TreeStorage) ExpressionPointer[ParameterCount];
    }

    for (unsigned i = 0; i < ParameterCount; i++)
    {
        BoundArguments[i] = NULL;
    }

    bool TargetIsPropertyAssignment = IsPropertySet(TargetProcedure);

    // If the procedure has a ParamArray parameter, create a list of arguments
    // to match it.

    unsigned ParamArrayIndex = 0;
    ExpressionList *ParamArrayElements = NULL;
    ExpressionList **ParamArrayTarget = NULL;
    Parameter *ParamArrayParameter = !HasFlag(OvrldFlags, OvrldIgnoreParamArray) ? TargetProcedure->GetParamArrayParameter() : NULL;

    if (ParamArrayParameter)
    {
        ParamArrayTarget = &ParamArrayElements;
        ParamArrayIndex = ParameterCount - 1;

        // A property assignment method has a parameter after the paramarray parameter.

        if (TargetIsPropertyAssignment)
        {
            ParamArrayIndex--;
        }
    }

    ILTree::Expression *RemainingArguments = Arguments;

    if (((OvrldFlags & OvrldSomeCandidatesAreExtensionMethods) && ! CandidateIsExtensionMethod))
    {
        ThrowIfNull(RemainingArguments);
        RemainingArguments = RemainingArguments->AsExpressionWithChildren().Right;
    }

    // Match the positional arguments to the first n parameters.

    unsigned ParameterIndex = 0;
    unsigned ArgumentCount = 0;

    while (RemainingArguments &&
           (RemainingArguments->AsExpressionWithChildren().Left == NULL ||
            !HasFlag32(RemainingArguments->AsExpressionWithChildren().Left, SXF_ARG_NAMED)))
    {
        ArgumentCount++;

        if (ParameterIndex == ParameterCount)
        {
            if (!SomeArgumentsBad)
            {
                if (m_ReportErrors)
                {
                    if (SuppressMethodNameInErrorMessages)
                    {
                        ReportSemanticError
                        (
                            ERRID_TooManyArgs,
                            RemainingArguments->Loc
                        );
                    }
                    else
                    {
                        StringBuffer textBuffer;

                        Declaration * pTarget = TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages);

                        ReportSemanticError
                        (
                            CandidateIsExtensionMethod ?
                                ERRID_TooManyArgs2 :
                                ERRID_TooManyArgs1,
                            RemainingArguments->Loc,
                            ExtractErrorName
                            (
                                pTarget,
                                textBuffer,
                                CandidateIsExtensionMethod,
                                FixedTypeArgumentBitVector,
                                GenericBindingContext.GetGenericBindingForErrorText()
                            ),
                            pTarget->GetContainer()->GetQualifiedName()
                        );
                    }
                }

                SomeArgumentsBad = true;
                ArgumentArityBad = true;
                //The number of arguments does not match, so we definetly want to give extension methods
                //the oportunity to bind rather than just reporting errors about instance methods.
                RequireInstanceMethodBinding = false;

                if (CheckValidityOnly)
                {
                    return NULL;
                }
            }
        }
        else if (ParameterIndex == ParamArrayIndex && ParamArrayTarget)
        {
            // Collect all the arguments that match the ParamArray parameter.

            while (RemainingArguments &&
                   (!TargetIsPropertyAssignment || RemainingArguments->AsExpressionWithChildren().Right) &&
                   (RemainingArguments->AsExpressionWithChildren().Left == NULL ||
                    !HasFlag32(RemainingArguments->AsExpressionWithChildren().Left, SXF_ARG_NAMED)))
            {
                if (RemainingArguments->AsExpressionWithChildren().Left)
                {
                    *ParamArrayTarget =
                        AllocateExpression(
                            SX_LIST,
                            TypeHelpers::GetVoidType(),
                            RemainingArguments->AsExpressionWithChildren().Left->AsArgumentExpression().Left,
                            NULL,
                            RemainingArguments->Loc);
                    ParamArrayTarget = &(*ParamArrayTarget)->AsExpressionWithChildren().Right;
                }
                else
                {
                    ReportSemanticError(
                        ERRID_OmittedParamArrayArgument,
                        RemainingArguments->Loc);

                    SomeArgumentsBad = true;
                    //The omitted argument matches a param array parameter.
                    //There may be an extension method with a signature for which this is not true
                    //so we give extension methods a chance to bind.
                    RequireInstanceMethodBinding = false;

                }

                RemainingArguments = RemainingArguments->AsExpressionWithChildren().Right;
            }

            break;
        }
        else
        {
            // For a property assignment, the assignment source value (which is always the
            // last argument) matches the last parameter.

            if (TargetIsPropertyAssignment &&
                RemainingArguments->AsExpressionWithChildren().Right == NULL)
            {
                VSASSERT(
                    ParameterIndex + 1 <= ParameterCount,
                    "Parameter matching for property assignment lost.");

                ParameterIndex = ParameterCount - 1;
            }

            BoundArguments[ParameterIndex++] = RemainingArguments->AsExpressionWithChildren().Left;
        }

        RemainingArguments = RemainingArguments->AsExpressionWithChildren().Right;
    }

    // Match the named arguments.

    unsigned FirstEligibleNamedIndex = ParameterIndex;

    while (RemainingArguments)
    {
        ILTree::Expression *Argument = RemainingArguments->AsExpressionWithChildren().Left;

        if (HasFlag32(Argument, SXF_ARG_NAMED))
        {
            Parameter *NamedParameter;

            if (TargetProcedure->GetNamedParam(Argument->AsArgumentExpression().Name->AsArgumentNameExpression().Name, &NamedParameter, &ParameterIndex))
            {
                if (NamedParameter->IsParamArray())
                {
                    ReportSemanticError(
                        ERRID_NamedParamArrayArgument,
                        Argument->AsArgumentExpression().Name->Loc);

                    SomeArgumentsBad = true;
                    //We give extension methods a chance to bind
                    RequireInstanceMethodBinding = false;
                }
                //The first argument of an extension method cannot be used as
                //a named parameter
                else if (CandidateIsExtensionMethod && ParameterIndex == 0)
                {
                    if (m_ReportErrors)
                    {
                        StringBuffer textBuffer;

                        ReportMethodCallError
                        (
                            SuppressMethodNameInErrorMessages,
                            CandidateIsExtensionMethod,
                            false, // No special delegate overload error message.
                            ERRID_NamedParamNotFound1,
                            ERRID_NamedParamNotFound2,
                            ERRID_NamedParamNotFound3,
                            ERRID_None, // No special delegate overload error message.
                            Argument->AsArgumentExpression().Name->Loc,
                            Argument->AsArgumentExpression().Name->Name,
                            TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages),
                            FixedTypeArgumentBitVector,
                            GenericBindingContext.GetGenericBindingForErrorText()
                        );
                    }

                    SomeArgumentsBad = true;
                    //Obviously if we are binding to an exension method
                    //we will not prevent extension method binding.
                    //Therefore we set RequireInstanceMethodBinding to false.
                    RequireInstanceMethodBinding = false;
                }
#pragma prefast (suppress: 26017, "Buffer access is correctly bounded")
                else if (BoundArguments[ParameterIndex] == NULL &&
                         // It is an error for a named argument to specify the source
                         // value of the assignment.
                         !(TargetIsPropertyAssignment && ParameterIndex == ParameterCount - 1))
                {
                    // It is an error for a named argument to specify
                    // a value for an explicitly omitted positional argument.
                    if (ParameterIndex < FirstEligibleNamedIndex)
                    {
                        if (m_ReportErrors)
                        {
                            ReportMethodCallError
                            (
                                SuppressMethodNameInErrorMessages,
                                CandidateIsExtensionMethod,
                                false, // No special message for delegates
                                ERRID_NamedArgAlsoOmitted1,
                                ERRID_NamedArgAlsoOmitted2,
                                ERRID_NamedArgAlsoOmitted3,
                                ERRID_None,
                                Argument->AsArgumentExpression().Name->Loc,
                                Argument->AsArgumentExpression().Name->Name,
                                TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages),
                                FixedTypeArgumentBitVector,
                                GenericBindingContext
                            );
                        }

                        SomeArgumentsBad = true;
                        //A named argument was specified that matched an omitted positional argument
                        //There may be an extension method signature for which this does not happen so we
                        //need to give them the oportunity to bind.
                        RequireInstanceMethodBinding = false;

                    }

#pragma prefast (suppress: 26017, "Buffer access is correctly bounded")
                    BoundArguments[ParameterIndex] = Argument;
                }
                else
                {
                    if (m_ReportErrors)
                    {
                        StringBuffer textBuffer;

                        ReportMethodCallError
                        (
                            SuppressMethodNameInErrorMessages,
                            CandidateIsExtensionMethod,
                            false, // No special error for delegates
                            ERRID_NamedArgUsedTwice1,
                            ERRID_NamedArgUsedTwice2,
                            ERRID_NamedArgUsedTwice3,
                            ERRID_None, // No special error for delegates
                            Argument->AsArgumentExpression().Name->Loc,
                            Argument->AsArgumentExpression().Name->Name,
                            TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages),
                            FixedTypeArgumentBitVector,
                            GenericBindingContext
                        );

                    }

                    SomeArgumentsBad = true;
                    //Using a named argument twice will be an error for both instance methods
                    //and for extension methods. Therefore, it's ok if we show extension method
                    //candidates in the list.
                    RequireInstanceMethodBinding = false;

                }
            }
            else
            {
                if (m_ReportErrors)
                {
                    StringBuffer textBuffer;

                    ReportSemanticError
                    (
                        SuppressMethodNameInErrorMessages ?
                            ERRID_NamedParamNotFound1 :
                            ERRID_NamedParamNotFound2,
                        Argument->AsArgumentExpression().Name->Loc,
                        Argument->AsArgumentExpression().Name->Name,
                        ExtractErrorName
                        (
                            TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages),
                            textBuffer,
                            CandidateIsExtensionMethod,
                            FixedTypeArgumentBitVector,
                            GenericBindingContext.GetGenericBindingForErrorText()
                        )
                    );
                }

                SomeArgumentsBad = true;
                //We definetly want to allow extension methods to bind if
                //we specify a named parameter that does not exist because that
                //name may exist in the signatures of extension methods.
                RequireInstanceMethodBinding = false;
            }
        }
        else
        {
            // It is valid for a single unnamed argument to follow named
            // arguments in a property assignment. This argument matches the
            // last parameter.

            if (TargetIsPropertyAssignment &&
                BoundArguments[ParameterCount - 1] == NULL)
            {
                BoundArguments[ParameterCount - 1] = Argument;
            }
            else
            {
                // If an unnamed argument follows named arguments for other than
                // a property assignment, the parser will have detected an error.

                SomeArgumentsBad = true;
                //This shouldn't happen. For completeness, however, we set
                //RequireInstanceMethodBinding to false...
                RequireInstanceMethodBinding = false;
            }
        }

        if (CheckValidityOnly && SomeArgumentsBad)
        {
            return NULL;
        }

        RemainingArguments = RemainingArguments->AsExpressionWithChildren().Right;
    }

    // Perform generic type argument inference on the arguments.
    // This must be completed for all arguments prior to type checking any arguments.

    GenericBindingInfo PrevGenericBindingContext = GenericBindingContext;
    Location *InferredTypeArgumentsLocations = NULL;

    // 
    // Port SP1 CL 2941063 to VS10
    // 







    bool originalReportErrors = m_ReportErrors;
    
    BackupValue<bool> backup_report_errors(&m_ReportErrors);
    m_ReportErrors = false;

    bool TypeInferenceSucceeded;

    AssertIfTrue(HasFlag(OvrldFlags, OvrldDisableTypeArgumentInference)); //DevDiv Bugs #22091 bad things may happen if we don't do the inference here

    TypeInferenceSucceeded =
        InferTypeArguments
        (
            CallLocation,
            TargetProcedure,
            BoundArguments,
            ParamArrayElements,
            DelegateReturnType,
            OvrldFlags,
            GenericBindingContext,
            InferredTypeArgumentsLocations,
            TypeInferenceLevel,                 // ByRef used for overload resolution to prevent regression from better inference.
            AllFailedInferenceIsDueToObject,
            false,
            SuppressMethodNameInErrorMessages,  //Even though error reporting is turned off here, we still pass in these two flags
            CandidateIsExtensionMethod,       //incase somone modified the code to turn error reporting on at some point in the future.
            originalReportErrors,
            ppAsyncSubArgumentListAmbiguity
        );

    backup_report_errors.Restore();

    InferenceFailed = !TypeInferenceSucceeded;

    // For procedures that cannot accept the number of supplied arguments, the not enough arguments
    // error has higher priority than type inference failure errors because the missing arguments
    // might be causing the type inference failures. 




    if (!TypeInferenceSucceeded && (m_ReportErrors || HasFlag(CallFlags, ExprCreateColInitElement)) &&
        !HasFlag(OvrldFlags, OvrldReportErrorsForAddressOf))
    {
        // Note that the perf hit for extra iteration over the parameters only occurs in error
        // scenarios, i.e. when type inference has already failed.

        ParameterIndex = 0;
        bool OmittedArgumentsDetected = false;

        for (Parameter *Param = TargetProcedure->GetFirstParam();
            Param && ParameterIndex < ParameterCount;
            Param = Param->GetNext(), ParameterIndex++)
        {
            if (Param->IsParamArray() ||
                Param->IsOptional() ||
                BoundArguments[ParameterIndex])
            {
                continue;
            }

            if (HasFlag(CallFlags, ExprIsLHSOfObjectInitializer))
            {
                AssertIfFalse(TargetProcedure->IsPropertySet());

                ReportSemanticError(
                    ERRID_ParameterizedPropertyInAggrInit1,
                    CallLocation,
                    TargetProcedure->IsPropertySet() ?
                        TargetProcedure->GetAssociatedPropertyDef()->GetName() :
                        TargetProcedure->GetName());

                OmittedArgumentsDetected = true;
                break;
            }

            StringBuffer textBuffer;

            ReportMethodCallError
            (
                SuppressMethodNameInErrorMessages,
                CandidateIsExtensionMethod,
                false, // No special error for addressof
                ERRID_OmittedArgument1,
                ERRID_OmittedArgument2,
                ERRID_OmittedArgument3,
                ERRID_None, // no special error for addressof
                CallLocation,
                Param->GetName(),
                TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages),
                FixedTypeArgumentBitVector,
                GenericBindingContext
            );

            OmittedArgumentsDetected = true;
        }

        if (OmittedArgumentsDetected)
        {
            ArgumentArityBad = true;
            SomeArgumentsBad = true;
            //Type inference failed due to missing arguments
            //we need to allow extension methods to bind
            //because they may match the number of arguments
            RequireInstanceMethodBinding = false;
        }

        if (SomeArgumentsBad)
        {
            return NULL;
        }
    }

    if (TypeInferenceSucceeded)
    {
        // Port SP1 CL # 2929420 to VS10.
        // In Whidbey, we would never check constraints if GenericBindingContext == PrevGenericBindingContext.
        // In Orcas, we have to check these constraints for extension method scenarios. However, in certain
        // cases, we should not check constraints if we don't need to (like if no types were inferred).
        // This is why we check if the binding context is only a type binding.
        // If we do not do this, it's possible to be in a case where we call CheckGenericConstraints, only
        // checking the type constraints, and not emitting an error.
        // See ddb 156803 for more details, and Microsoft has the full details of this 
        if (!(GenericBindingContext.IsNull() || GenericBindingContext.IsGenericTypeBinding()))
        {
            Location * TypeArgumentLocations = NULL;
            if (GenericBindingContext.GetTypeArgumentLocations() != NULL)
            {
                TypeArgumentLocations = GenericBindingContext.GetTypeArgumentLocations();
            }
            else if (InferredTypeArgumentsLocations)
            {
                TypeArgumentLocations = InferredTypeArgumentsLocations;
            }
            else
            {
                GenericBinding *GenericBinding = GenericBindingContext.PGenericBinding();
                unsigned TypeArgumentCount = GenericBinding->GetArgumentCount();

                // If we are reporting overload resolution but don't have a proper location (AKA we inferred the resulting types),
                // just force reporting of error on the calling site if inference changed the binding.

                // Dev10#409302 -- This way of judging whether or not to report errors is AWFUL.
                // The decision about whether or not to report errors is a serious issue that the
                // callers must decide, e.g. like it does through the flag m_ReportErrors. It can't be left
                // to inscrutable hacks in the callee.
                // In the following it used to suppress error reporting if GenericBindingContext==PrevGenericBindingContext,
                // i.e. if the call to InferTypeArguments happened to add to GenericBindingContext.
                // I removed that suppression on the grounds that m_ReportErrors is a better way.
                // This removal has not broken any suites. So I think that the suppression wasn't even important.

                if (TypeArgumentCount > 0 && CallLocation.IsValid() && !CallLocation.IsHidden())
                {
                    TypeArgumentLocations = (Location *)m_TreeStorage.Alloc(sizeof(Location) * TypeArgumentCount);
                    for (unsigned i = 0; i < TypeArgumentCount; i++)
                    {
                        TypeArgumentLocations[i] = CallLocation;
                    }
                }
            }

            if (!Bindable::CheckGenericConstraints
            (
                GenericBindingContext.PGenericBinding(),
                TypeArgumentLocations,
                NULL,
                m_ReportErrors && (TypeArgumentLocations || SuppressMethodNameInErrorMessages)? m_Errors : NULL, // Only report errors if we should, and if we have an error location (unless we don't need aka for overload resolution reporting)
                m_CompilerHost,
                m_Compiler,
                &m_SymbolCreator,
                m_CompilationCaches
            ))
            {
                SomeArgumentsBad = true;
                return NULL;
            }
        }

    }
    else  // Inference failed
    {
        // Type inference failed. But try to infer again so that errors are reported.
        //
        TypeInferenceSucceeded =
            InferTypeArguments
            (
                CallLocation,
                TargetProcedure,
                BoundArguments,
                ParamArrayElements,
                DelegateReturnType,
                OvrldFlags,
                PrevGenericBindingContext,
                InferredTypeArgumentsLocations,
                TypeInferenceLevel,
                AllFailedInferenceIsDueToObject,
                false,
                SuppressMethodNameInErrorMessages,
                CandidateIsExtensionMethod
            );

        VSASSERT(!TypeInferenceSucceeded, "Type inference inconsistency detected!!!");

        // Port SP1 CL 2943055 to VS10
        // 

        if ( m_ReportErrors && m_Errors && !m_Errors->HasErrors() )
        {
            ReportSemanticError(ERRID_InternalCompilerError, CallLocation);
        }

        if (!AllFailedInferenceIsDueToObject)
        {
            SomeArgumentsBad = true;
            //We have an early bound type infernece failure.
            //We allow extension methods to bind because inference
            //may not fail for them.
            RequireInstanceMethodBinding = false;
        }

        // The parameter and return types of the target procedure are effectively not well formed, and
        // so type checking the arguments is not possible.

        return NULL;
    }

    // Traverse the parameters, converting corresponding arguments
    // as appropriate.

    ParameterIndex = 0;

    Assume(!GenericBindingContext.IsPartialBinding(), L"We should not have a partial generic binding context here!");

    Parameter * Param = TargetProcedure->GetFirstParam();

    if (CandidateIsExtensionMethod)
    {
        Param = Param->GetNext();
        ParameterIndex +=1;
    }

    for
    (
        ;
        Param;
        Param = Param->GetNext(), ParameterIndex++
    )
    {
        Type *RawTargetType = GetDataType(Param);
        bool IsByref = false;

        // ByRef parameters are marked in the symbol table as
        // having pointer types.

        if (TypeHelpers::IsPointerType(RawTargetType))
        {
            IsByref = true;
            RawTargetType = TypeHelpers::GetReferencedType(RawTargetType->PPointerType());
        }
        else if (Param->IsByRefKeywordUsed())
        {
            IsByref = true;
        }

        Type *TargetType = RawTargetType;

        if (!GenericBindingContext.IsNull())
        {
            TargetType = ReplaceGenericParametersWithArguments(TargetType, GenericBindingContext.PGenericBinding(), m_SymbolCreator);
        }

        if (Param->IsParamArray() && !HasFlag(OvrldFlags, OvrldIgnoreParamArray))
        {
            if (!TypeHelpers::IsArrayType(TargetType))
            {
                ReportSemanticError(
                    ERRID_ParamArrayWrongType,
                    CallLocation);

                SomeArgumentsBad = true;
                //We have a param array parameter with a non array type.
                //We definetly need to allow extension methods to bind.
                RequireInstanceMethodBinding = false;
                continue;
            }

            // If exactly one argument matches the ParamArray parameter, and the
            // type of the argument is an array type that conforms to the type of
            // the ParamArray parameter, pass the argument along.
            //
            // A Nothing literal is treated as a matching array type.

            if (ParamArrayElements &&
                ParamArrayElements->AsExpressionWithChildren().Right == NULL &&
                !IsBad(ParamArrayElements->AsExpressionWithChildren().Left) &&
                CanPassToParamArray(ParamArrayElements->AsExpressionWithChildren().Left, TargetType) &&
                !DisallowParamArrayExactMatch)
            {
                if (IsByref && ParamArrayElements->AsExpressionWithChildren().Left->bilop == SX_SYM)
                {
                    SetFlag32(ParamArrayElements->AsExpressionWithChildren().Left,  SXF_SYM_PASSEDBYREF);
                }

                //As far as I can tell, this should not fail, because CanPassToParamArray above
                //requires the conversion from the Argument type to the param array type to
                //either be widening or identity.

                ILTree::Expression *ParamArray =
                    ConvertWithErrorChecking
                    (
                        ParamArrayElements->AsExpressionWithChildren().Left,
                        TargetType,
                        ExprNoFlags,
                        SuppressMethodNameInErrorMessages
                    );

                if (!IsBad(ParamArray) && IsByref)
                {
                    ParamArray =
                        PassArgumentByref
                        (
                            ParamArray,
                            Param,
                            TargetType,
                            CheckValidityOnly,
                            RejectNarrowingConversions,
                            TargetIsDllDeclare,
                            CopyOutArguments,
                            SomeArgumentsBad,
                            RequiresNarrowingConversion,
                            RequiresSomeConversion,
                            AllNarrowingIsFromObject,
                            AllNarrowingIsFromNumericLiteral,
                            SuppressMethodNameInErrorMessages,
                            DelegateRelaxationLevel,
                            RequiresUnwrappingNullable,
                            RequireInstanceMethodBinding
                        );
                }

                if (IsBad(ParamArray))
                {

                    //If a failure happened somwhere inside PassArgumentByRef then
                    //it would set RequireInstanceMethodBinding when necessary.
                    //As a result we don't need to set it here.
                    //But, because we got baddnesss out as a result we do
                    //set SomeArgumentsBad to true so that our caller will
                    //mark the method as not callable.
                    SomeArgumentsBad = true;
                }

                BoundArguments[ParameterIndex] = ParamArray;

                // The paramarray is not necessarily the last parameter
                // (the method may be a Property Set), so don't terminate
                // the argument-processing loop.

                continue;
            }

            // For overload resolution, we may need to disallow paramarray expansion
            // when checking for exact matches.
            if (DisallowParamArrayExpansion)
            {
                VSASSERT(CheckValidityOnly, "This only works for validity checking!");

                SomeArgumentsBad = true;
                //We have a non expanded candidate
                //but the arguments require us to expand.
                //This should not prevent extension methods from binding.
                RequireInstanceMethodBinding = false;
                return NULL;
            }
            else if (DisallowParamArrayExactMatch)
            {
                VSASSERT(CheckValidityOnly, "This only works for validity checking!");

                // Nothing passed to a paramarray will widen to both the type of the
                // paramarray and the array type of the paramarray. In that case, we
                // explicitly prefer the unexpanded paramarray.
                if (ParamArrayElements &&
                    ParamArrayElements->AsExpressionWithChildren().Right == NULL &&
                    !IsBad(ParamArrayElements->AsExpressionWithChildren().Left) &&
                    IsNothingLiteral(ParamArrayElements->AsExpressionWithChildren().Left))
                {
                    SomeArgumentsBad = true;
                    //We have an expanded candidate by the
                    //arguments require us to expand.
                    //This should not prevent extension methods from binding.
                    RequireInstanceMethodBinding = false;
                    return NULL;
                }

            }

            // Otherwise, for a ParamArray parameter, all the matching arguments are passed
            // ByVal as instances of the element type of the ParamArray.

            // Perform the conversions to the element type of the ParamArray here, though
            // InitializeArray would perform them, in order to collect information about
            // narrowing conversions and to avoid generating spurious temporaries in
            // overload resolution.

            Type *ParamArrayElementType = TypeHelpers::GetElementType(TargetType->PArrayType());

            for (ILTree::Expression *ParamArrayElement = ParamArrayElements;
                 ParamArrayElement;
                 ParamArrayElement = ParamArrayElement->AsExpressionWithChildren().Right)
            {
                ILTree::Expression *ElementValue = ParamArrayElement->AsExpressionWithChildren().Left;

                ParamArrayElement->AsExpressionWithChildren().Left =
                    PassArgumentByval
                    (
                        ParamArrayElement->AsExpressionWithChildren().Left,
                        Param,
                        ParamArrayElementType,
                        CallFlags,
                        CheckValidityOnly,
                        RejectNarrowingConversions,
                        SomeArgumentsBad,
                        RequiresNarrowingConversion,
                        RequiresSomeConversion,
                        AllNarrowingIsFromObject,
                        AllNarrowingIsFromNumericLiteral,
                        SuppressMethodNameInErrorMessages,
                        DelegateRelaxationLevel,
                        RequiresUnwrappingNullable,
                        RequireInstanceMethodBinding
                    );
            }

            if (!CheckValidityOnly && !SomeArgumentsBad)
            {
                ILTree::Expression *ParamArray =
                    InitializeArray(
                        ParamArrayElements,
                        TargetType->PArrayType(),
                        NULL,
                        ParamArrayElements ? ParamArrayElements->Loc : CallLocation);

                // The param array may be ByRef if it comes from a COM library.
                // If so, we need to load the address of it.

                if (!IsBad(ParamArray) && IsByref)
                {
                    if (ParamArray->bilop == SX_SEQ_OP2)
                    {
                        // If the param array has been captured to a temporary, then
                        // take the address of the temporary.

                        ParamArray->AsExpressionWithChildren().Right =
                            MakeAddress(ParamArray->AsExpressionWithChildren().Right, true);
                    }
                    else
                    {
                        ParamArray = MakeAddress(ParamArray, true);
                    }
                }

                if (IsBad(ParamArray))
                {
                    //We got back a bad result when we tried to construct the
                    //param array argument. We mark the procedure as not callable.
                    //However, we do not explictly set RequireInstanceMethodBinding to false.
                    //This is because PassArgumentByVal will do this if the particular failure that
                    //caused it to report badness requires this.
                    SomeArgumentsBad = true;
                }

                BoundArguments[ParameterIndex] = ParamArray;

                // There are some cases, e.g. property assignment, in which a ParamArray
                // parameter is not necessarily last, so terminating the loop at this
                // point would be incorrect.
            }

            continue;
        }

        bool IsByvalUsingByrefMechanism = false;

        // ByVal String parameters to DLL declares are passed byref--not byval using a
        // byref mechanism, but truly byref. (However, if the supplied argument is not
        // a String, byval conversion rules apply.) Only do this if it doesn't have
        // explicit marshalling information.

        if (TargetIsDllDeclare &&
            !Param->GetPWellKnownAttrVals()->GetMarshalAsData() &&
            TypeHelpers::IsStringType(TargetType) &&
            !IsByref)
        {
            IsByref = true;
            IsByvalUsingByrefMechanism = true;
        }

        ILTree::Expression *Argument = NULL;

        if (BoundArguments[ParameterIndex])
        {
            ILTree::ArgumentExpression &ArgumentHolder =
                BoundArguments[ParameterIndex]->AsArgumentExpression();
            Argument = ArgumentHolder.Left;

            AsyncSubAmbiguityFlags AsyncSubArgumentAmbiguity = FoundNoAsyncOverload;

            if (IsBad(Argument))
            {
            }
            else if (TypeHelpers::IsBadType(TargetType))
            {
                ReportBadType(TargetType, Argument->Loc);
                MakeBad(Argument);
            }
            else if (IsByvalUsingByrefMechanism)
            {
                // Making the argument into an RValue causes its value to be (ultimately)
                // converted to the target type and copied to a temporary, with the
                // temporary being passed byref.
                //
                // The rule (inherited from the past) seems to be that an lvalue
                // with matching type is actually passed ByRef.

                if (!TypeHelpers::EquivalentTypes(TargetType, Argument->ResultType))
                {
                    if (/*IsByref && */ Argument->bilop == SX_SYM)
                    {
                        SetFlag32(Argument,  SXF_SYM_PASSEDBYREF);
                    }
                    Argument = MakeRValue(Argument);
                }
            }
            else if (!IsByref)
            {
                Argument =
                    PassArgumentByval
                    (
                        Argument,
                        Param,
                        TargetType,
                        CallFlags,
                        CheckValidityOnly,
                        RejectNarrowingConversions,
                        SomeArgumentsBad,
                        RequiresNarrowingConversion,
                        RequiresSomeConversion,
                        AllNarrowingIsFromObject,
                        AllNarrowingIsFromNumericLiteral,
                        SuppressMethodNameInErrorMessages,
                        DelegateRelaxationLevel,
                        RequiresUnwrappingNullable,
                        RequireInstanceMethodBinding,
                        &AsyncSubArgumentAmbiguity
                    );
            }

            if (AsyncSubArgumentAmbiguity != FoundNoAsyncOverload)
            {
                AddAsyncSubArgumentAmbiguity(ppAsyncSubArgumentListAmbiguity, &ArgumentHolder, AsyncSubArgumentAmbiguity);
            }

        }

        else if (Param->IsOptional() && !HasFlag(OvrldFlags, OvrldExactArgCount))
        {
            UsedDefaultForAnOptionalParameter = true;
        
            if (TypeHelpers::IsBadType(TargetType))
            {
                ReportBadType(TargetType, CallLocation);
                Argument = AllocateBadExpression(CallLocation);
            }

            // OPTIONAL: this is where optional parameters are turned into arguments
            else if (TypeHelpers::IsRootObjectType(TargetType) && !Param->IsParamWithValue())
            {
                // An Optional Parameter with no default value signifies a COM call
                // with one of two param types, a VB6 Variant or VB6 Object.
                // Microsoft 2009.01.18: ...no it doesn't! It only does if the type is a COM type.
                // If it's an Object we load Nothing.  For Variant, we load
                // System.Missing.Value unless a custom attribute exists directing us
                // to make a Dispatch or Unknown wrapper.  Metaimport has already determined
                // this information for us.

                if (Param->IsMarshaledAsObject())
                {
                    Argument = AllocateExpression(SX_NOTHING, TargetType, CallLocation);
                }
                else if ((Param->GetPWellKnownAttrVals()->GetIDispatchConstantData() &&
                            GetFXSymbolProvider()->IsTypeAvailable(FX::DispatchWrapperType)) || // Certain platforms do not support dispatch wrappers.
                         (Param->GetPWellKnownAttrVals()->GetIUnknownConstantData() &&
                            GetFXSymbolProvider()->IsTypeAvailable(FX::UnknownWrapperType)))    // Certain platforms do not support unknown wrappers.
                {
                    // In the situation that COM needs a VT_DISPATCH or VT_UNKNOWN default value,
                    // (which is determined by the presence of either a IDispatchConstantAttribute
                    // or IUnknownConstantAttribute), we need to construct the appropriate wrapper
                    // class so it will get marshalled correctly.

                    Type *TypeOfInstance =
                        Param->GetPWellKnownAttrVals()->GetIDispatchConstantData() ?
                            GetFXSymbolProvider()->GetType(FX::DispatchWrapperType) :
                            GetFXSymbolProvider()->GetType(FX::UnknownWrapperType);

                    Argument =
                        CreateConstructedInstance(
                            TypeOfInstance,
                            CallLocation,
                            CallLocation,
                            AllocateExpression(
                                SX_LIST,
                                TypeHelpers::GetVoidType(),
                                AllocateExpression(
                                    SX_ARG,
                                    TypeHelpers::GetVoidType(),
                                    AllocateExpression(
                                        SX_NOTHING,
                                        GetFXSymbolProvider()->GetObjectType(),
                                        CallLocation),
                                    CallLocation),
                                CallLocation),
                            false,
                            ExprNoFlags);

                }
                else
                {
                    Argument = MakeMissingArgument(CallLocation);
                }

                if (IsBad(Argument))
                {
                    //If we are unable to construct the default value for an optional argument
                    //when we need it, then the method is definitely not callable, so we set
                    //SomeArgumentsBad to true. However, that failure is due to an inability
                    //to bind to a constructor, not to an incompatability between the arguments and the method
                    //signature. In that case it's more appropriate to bind to the instance method and generate an error
                    //then to bind to an extension method. Therefore we do not explicitly set
                    //RequireInstanceMethodBinding to false (although other failures in the singature may do so, this one doesn't).
                    SomeArgumentsBad = true;
                    continue;
                }
            }

            else
            {
                ConstantValue Value;

                if (Param->IsParamWithValue())
                {
                    // If the argument has the OptionCompareAttribute
                    // then use the setting for Option Compare [Binary|Text]
                    // Other languages will use the default value specified.
                    if (Param->IsOptionCompare())
                    {
                        Value.TypeCode = t_i4;
                        if ( m_SourceFileOptions & OPTION_OptionText )
                        {
                            Value.Integral = 1;
                        }
                        else
                        {
                            Value.Integral = 0;
                        }
                    }
                    else
                    {

                        Value = GetConstantValue(CallLocation, Param->PParamWithValue()->GetExpression());

                        if (!IgnoreCallerInfoAttribute())
                        {
                            ConstantValue  CallerInfoValue;

                            bool hasUn----edCallerLineNumberAttribute = false;
                            bool hasUn----edCallerMemberNameAttribute = false;
                            bool hasUn----edCallerFilePathAttribute = false;

                            // Conditions to check unbound(but resolved) CallerInfo
                            // 1. Currently is binding an attribute
                            // 2. Method of the param is a constructor
                            // 3. Class of the param is not bound

                            if (// 1
                                m_NamedContextForAppliedAttribute && 
                                // 2
                                Param->GetPAttrVals() && Param->GetPAttrVals()->GetPsymContextOfParamWithApplAttr()->IsProc() &&
                                Param->GetPAttrVals()->GetPsymContextOfParamWithApplAttr()->PProc()->IsAnyConstructor() &&
                                // 3
                                Param->GetPAttrVals()->GetPsymContextOfParamWithApplAttr()->PProc()->GetParent() &&
                                Param->GetPAttrVals()->GetPsymContextOfParamWithApplAttr()->PProc()->GetParent()->IsClass() &&
                                ! Param->GetPAttrVals()->GetPsymContextOfParamWithApplAttr()->PProc()->GetParent()->PClass()->IsBindingDone())
                            {
                                BCSYM_ApplAttr* pNon----edAttribute = Param->GetPAttrVals() ? Param->GetPAttrVals()->GetPNon----edData()->m_psymApplAttrHead : NULL;

                                while (pNon----edAttribute)
                                {       
                                    BCSYM_NamedType *pAttrClass = pNon----edAttribute->GetAttrClass();

                                    if (pAttrClass && 
                                        pAttrClass->GetSymbol() &&
                                        pAttrClass->GetSymbol()->IsClass())
                                    {
                                        STRING *strName = ConcatNameSpaceAndName(
                                            pAttrClass->GetSymbol()->PClass()->GetCompiler(),
                                            pAttrClass->GetSymbol()->PClass()->GetNameSpace(),
                                            pAttrClass->GetSymbol()->PClass()->GetName());

                                        if (CompareNoCase(CALLERLINENUMBERATTRIBUTE, strName) == 0)
                                        {
                                            hasUn----edCallerLineNumberAttribute = true;
                                        }
                                        else if (CompareNoCase(CALLERMEMBERNAMEATTRIBUTE, strName) == 0)
                                        {
                                            hasUn----edCallerMemberNameAttribute = true;
                                        }
                                        else if(CompareNoCase(CALLERFILEPATHATTRIBUTE, strName) == 0)
                                        {
                                            hasUn----edCallerFilePathAttribute = true;
                                        }
                                    }
                                    pNon----edAttribute = pNon----edAttribute->GetNext();
                                }
                            }

                            if ( ((Param->GetPWellKnownAttrVals() && Param->GetPWellKnownAttrVals()->GetCallerLineNumberData()) || 
                                hasUn----edCallerLineNumberAttribute) && 
                                pCallerInfoLineNumber != NULL )
                            {
                                if (pCallerInfoLineNumber->IsHidden())
                                {
                                    VSASSERT(false, "Why A hidden location can reach here?");
                                    CallerInfoValue.TypeCode = t_bad;  
                                }
                                else
                                {
                                    CallerInfoValue.Integral = pCallerInfoLineNumber->m_lEndLine + 1;
                                    CallerInfoValue.TypeCode = t_i4;
                                }
                            }
                            else if (Param->GetPWellKnownAttrVals() && Param->GetPWellKnownAttrVals()->GetCallerMemberNameData() || 
                                hasUn----edCallerMemberNameAttribute)                             
                            {                               
                                if (m_FieldInitializerContext != NULL)
                                {                                    
                                    CallerInfoValue.String.Spelling = m_FieldInitializerContext->GetName();                                 
                                    CallerInfoValue.String.LengthInCharacters = (unsigned)wcslen(CallerInfoValue.String.Spelling);
                                    CallerInfoValue.TypeCode = t_string;
                                }
                                else if (m_NamedContextForAppliedAttribute != NULL &&
                                    ! m_NamedContextForAppliedAttribute->IsContainer())
                                {
                                    BCSYM_Proc *pContainingProc = m_NamedContextForAppliedAttribute->IsProc() ? 
                                        m_NamedContextForAppliedAttribute->PProc() : NULL;

                                    if (pContainingProc &&
                                        (pContainingProc->IsPropertyGet() || pContainingProc->IsPropertySet()) &&
                                        pContainingProc->GetAssociatedPropertyDef() != NULL)
                                    {                                                                       
                                        CallerInfoValue.String.Spelling = pContainingProc->GetAssociatedPropertyDef()->GetName();                                       
                                    }
                                    else if (pContainingProc && pContainingProc->IsEventAccessor() &&   pContainingProc->CreatedByEventDecl())                                      
                                    {
                                        CallerInfoValue.String.Spelling = pContainingProc->CreatedByEventDecl()->GetName();
                                    }
                                    else
                                    {                                
                                        CallerInfoValue.String.Spelling = m_NamedContextForAppliedAttribute->GetName();                                 
                                    }

                                    CallerInfoValue.String.LengthInCharacters = (unsigned)wcslen(CallerInfoValue.String.Spelling);
                                    CallerInfoValue.TypeCode = t_string;
                                }
                                else if (m_Procedure != NULL )
                                {
                                    if ((m_Procedure->IsPropertyGet() || m_Procedure->IsPropertySet()) && 
                                        m_Procedure->GetAssociatedPropertyDef() != NULL)
                                    {
                                        CallerInfoValue.String.Spelling = m_Procedure->GetAssociatedPropertyDef()->GetName();                                       
                                    }
                                    else if( m_Procedure->IsEventAccessor() && m_Procedure->CreatedByEventDecl())
                                    {
                                        CallerInfoValue.String.Spelling = m_Procedure->CreatedByEventDecl()->GetName();
                                    }
                                    else
                                    {
                                        CallerInfoValue.String.Spelling = m_Procedure->GetName();
                                    }

                                    CallerInfoValue.String.LengthInCharacters = (unsigned)wcslen(CallerInfoValue.String.Spelling);
                                    CallerInfoValue.TypeCode = t_string;
                                }
                            }
                            else if (
                                ((Param->GetPWellKnownAttrVals() && Param->GetPWellKnownAttrVals()->GetCallerFilePathData()) || hasUn----edCallerFilePathAttribute) &&
                                m_SourceFile != NULL &&
                                m_SourceFile->GetFileName() != NULL)
                            {
                                CallerInfoValue.String.Spelling = m_SourceFile->GetFileName();
                                CallerInfoValue.String.LengthInCharacters = (unsigned)wcslen(CallerInfoValue.String.Spelling);
                                CallerInfoValue.TypeCode = t_string;
                            }

                            if (CallerInfoValue.TypeCode != t_bad)
                            {
                                BackupValue<bool> m_pReportErrorsBackup(&m_ReportErrors);
                                m_ReportErrors = false;
                                Argument = ProduceConstantExpression(
                                    CallerInfoValue,
                                    CallLocation,
                                    GetFXSymbolProvider()->GetType(CallerInfoValue.TypeCode)
                                    IDE_ARG(0));

                                Argument = ConvertWithErrorChecking(
                                    Argument,
                                    TargetType,
                                    ExprNoFlags,
                                    false, //SuppressMethodNameInErrorMessages
                                    NULL, //pRequiresUnwrappingNullable
                                    true // IgnoreOperatorMethod
                                    );

                                if (!IsBad(Argument))
                                {
                                    Value = CallerInfoValue;
                                }
                            }

                        }



                        if (Value.TypeCode == t_bad)
                        {
                            //There is a problem with the default value for an optional parameter.
                            //This clearly makes the method not callable, given the fact that
                            //we are calling the method with the optional parameter omitted.
                            //However this error should not allow us to access extension methods
                            //during overload resolution (other errors happening in the signature of the same call may allow
                            //extension methods, but this error does not). Therefore we set SomeArgumentsBad to true but
                            //do not set RequireInstanceMethodBinding to false.
                            SomeArgumentsBad = true;
                            continue;
                        }
                    }
                }
                else
                {
                    // Create a zero-valued constant.
                    // If the TargetType is a nullable or a structure or a generic, we want to set the Value.TypeCode
                    // to be t_ref (which implies "Nothing") because the default parameter value should be NOTHING.
                    // See also the comment at the start of ProduceConstantExpression
                    // OPTIONAL:
                    Value.TypeCode = (TargetType->GetVtype() == t_struct || TargetType->GetVtype() == t_generic) ? t_ref : TargetType->GetVtype();
                }

                // OPTIONAL:
                Type *TypeOfDefaultValue;
                if (RawTargetType->GetVtype() == t_generic)
                {
                    TypeOfDefaultValue = GetFXSymbolProvider()->GetObjectType();
                }
                else if (TargetType->GetVtype() == Value.TypeCode)
                {
                    TypeOfDefaultValue = TargetType;
                }
                else if (Value.TypeCode == t_ref && 
                         (TargetType->GetVtype() == t_string || // ByVal var As String = Nothing produces a NULL t_ref
                          TargetType->GetVtype() == t_array || // ByVal var() as anything = Nothing produces a NULL t_ref
                          TypeHelpers::IsNullableType(TargetType) || // ByVal var as T? = Nothing produces a NULL t_ref
                          TargetType->GetVtype() == t_struct)) // ByVal var as MyStruct = Nothing produces a NULL t_ref
                {
                    TypeOfDefaultValue = TargetType;
                    // Note: we're going to call ProduceConstantExpression.
                    // In that function, if Value.TypeCode is t_ref, then it produces NOTHING.
                    // If TargetType is a struct (e.g. MyStruct or Integer?) then it IGNORES the TypeOfDefaultValue
                    // and simply returns "Nothing" of type Object.
                    // Otherwise, it returns "Nothing" of type TypeOfDefaultValue.
                }
                else if (Value.TypeCode == t_ref)
                {
                    // This situation comes from e.g. Optional ByVal x as Boolean, but where the optional
                    // is encoded in the IL as .param=nullref. Such a case is never emitted by VB/C#.
                    // But it can be hand-written in IL, and we don't want to crash if it happens!
                    // So we pick "Object" type as the type of that default.
                    TypeOfDefaultValue = GetFXSymbolProvider()->GetObjectType();
                    // Microsoft 2009.01.18 - consider: the above cases "struct/nullable" should be rolled into this one,
                    // for clarity.
                }
                else
                {
                    TypeOfDefaultValue = GetFXSymbolProvider()->GetType(Value.TypeCode);
                }

                Argument =
                    ProduceConstantExpression(
                        Value,
                        CallLocation,
                        TypeOfDefaultValue
                        IDE_ARG(0));

                // If the type of the parameter is Object, then the type
                // of the optional value can be of another type. Make sure the
                // argument value is of the type of the parameter.

                Argument =
                    ConvertWithErrorChecking(
                        Argument,
                        TargetType,
                        ExprNoFlags);
            }
        }

        else
        {
            if (HasFlag(OvrldFlags, OvrldExactArgCount))
            {
                ReportSemanticError
                (
                    ERRID_NoArgumentCountOverloadCandidates1,
                    CallLocation,
                    TargetProcedure->IsPropertySet() ?
                        TargetProcedure->GetAssociatedPropertyDef()->GetName() :
                        TargetProcedure->GetName());

                SomeArgumentsBad = true;
                //We are requiring an extact argument count match for overload resolution,
                //yet this procedure has parameters that have not been specified.
                //This means that method is not callable, and so we mark it as such.
                //We also set RequireInstanceMethodBinding to false
                //because we need to give extension methods that have the correct arity
                //the oportunity to bind.
                RequireInstanceMethodBinding = false;
                break;
            }

            if (HasFlag(CallFlags, ExprIsLHSOfObjectInitializer))
            {
                AssertIfFalse(TargetProcedure->IsPropertySet());

                ReportSemanticError(
                    ERRID_ParameterizedPropertyInAggrInit1,
                    CallLocation,
                    TargetProcedure->IsPropertySet() ?
                        TargetProcedure->GetAssociatedPropertyDef()->GetName() :
                        TargetProcedure->GetName());

                //Parameterized properties cannot be initialized inside aggregate intializers.
                //This clearly makes the method not callable, and so we set SomeArgumentsBad = true.
                //This error will apply regardless of wether we bind to instance methods or to extension methods so
                //we give extension methods the chance to bind as well by setting RequireInstanceMethodBinding to false
                SomeArgumentsBad = true;
                RequireInstanceMethodBinding = false;
                break;
            }

            // 
            if (m_ReportErrors && !HasFlag(OvrldFlags, OvrldReportErrorsForAddressOf))
            {
                StringBuffer textBuffer;

                ReportMethodCallError
                (
                    SuppressMethodNameInErrorMessages,
                    CandidateIsExtensionMethod,
                    false, // No special error for addressof
                    ERRID_OmittedArgument1,
                    ERRID_OmittedArgument2,
                    ERRID_OmittedArgument3,
                    ERRID_None, // no special error for addressof.
                    CallLocation,
                    Param->GetName(),
                    TargetProcedureForErrorMessage(TargetProcedure, RepresentTargetInMessages),
                    FixedTypeArgumentBitVector,
                    GenericBindingContext
                );
            }


            //A non optional argument is missing. This makes the method not callable, so we set SomeArgumentsBad to true.
            //To give extension methods the chance to bind we also set RequireInstanceMethodBinding to false. This is
            //necessary because the extension methods may have different arities and hence could be a better match.
            SomeArgumentsBad = true;
            ArgumentArityBad = true;
            RequireInstanceMethodBinding = false;
            continue;
        }

        if (IsBad(Argument))
        {

            //Because at least one argument is Bad, we make sure to set
            //SomeArgumentsBad to true, hence marking the method as not callable.
            //We do not affect the value of RequireInstanceMethodBinding, however,
            //because at this point we have no clue why the argument was bad.
            //This shouldn't be a problem however, because RequireInstanceMethodBinding
            //should be set by the code above where necessary.
            SomeArgumentsBad = true;
        }

        else if (IsByref)
        {
            if (Argument->bilop == SX_SYM)
            {
                SetFlag32(Argument,  SXF_SYM_PASSEDBYREF);
            }
            Argument =
                PassArgumentByref
                (
                    Argument,
                    Param,
                    TargetType,
                    CheckValidityOnly,
                    RejectNarrowingConversions,
                    TargetIsDllDeclare,
                    CopyOutArguments,
                    SomeArgumentsBad,
                    RequiresNarrowingConversion,
                    RequiresSomeConversion,
                    AllNarrowingIsFromObject,
                    AllNarrowingIsFromNumericLiteral,
                    SuppressMethodNameInErrorMessages,
                    DelegateRelaxationLevel,
                    RequiresUnwrappingNullable,
                    RequireInstanceMethodBinding
                );
        }

        if (CheckValidityOnly && SomeArgumentsBad)
        {
            return NULL;
        }

        BoundArguments[ParameterIndex] = Argument;
    }

    if (CheckValidityOnly)
    {
        return NULL;
    }

    // Find the last supplied argument.

    ILTree::Expression *LastArgument = NULL;
    unsigned ArgumentIndex;

    for (ArgumentIndex = ParameterCount;
         LastArgument == NULL && ArgumentIndex != 0;
         ArgumentIndex--)
    {
        LastArgument = BoundArguments[ArgumentIndex - 1];
    }

    // Construct the bound argument list from the bound arguments.
    // The list is right-heavy, with each element having location information
    // spanning from the beginning of the argument through the end of the
    // argument list.

    ExpressionList *Result = NULL;

    if (LastArgument)
    {
        do
        {
            Result =
                (ExpressionList *)AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    BoundArguments[ArgumentIndex],
                    Result,
                    BoundArguments[ArgumentIndex] ?
                        BoundArguments[ArgumentIndex]->Loc :
                        Result->Loc,
                    LastArgument->Loc);

        } while (ArgumentIndex-- != 0);
    }

    return Result;
}


ILTree::Expression *
Semantics::GenerateNonPropertyAssignment
(
    const Location &AssignmentLocation,
    ILTree::Expression *Target,
    ILTree::Expression *Source
)
{
    // If we are assigning Nothing or a newly constructed instance to a Structure, then
    // just initialize the storage target.  This is cheaper than building an instance or
    // using the "Nothing" temporary (VSW#60949).
    //
    // Catch these three patterns:
    //     x = Nothing
    //     x = New s()
    //     d (As Date) = Nothing
    // and transform them into an INIT on x.  These will turn into an initobj opcode in the
    // code generator.

    Type *TargetType = Target->ResultType;

    if (!m_IsGeneratingXML &&
        HasFlag32(Target, SXF_LVALUE) &&  // It would be nice to assert this for all calls to this function, but Using variables are wierd in that
                                        // they are not marked as lvalues.  Just skip them.
        (TypeHelpers::IsRecordType(TargetType) ||    // Numerics are handled natively with opcodes.
            TypeHelpers::IsDateType(TargetType) ||
            TypeHelpers::IsDecimalType(TargetType))) 
    {
        // 

        if (Source->bilop == SX_SEQ_OP2 && Source->AsBinaryExpression().Left->bilop == SX_CALL)
        {
            ILTree::CallExpression *PossibleConstructorCall = &Source->AsBinaryExpression().Left->AsCallExpression();

            if (PossibleConstructorCall->Left->bilop == SX_SYM &&
                PossibleConstructorCall->Left->AsSymbolReferenceExpression().Symbol->IsProc() &&
                PossibleConstructorCall->Left->AsSymbolReferenceExpression().Symbol->PProc()->IsInstanceConstructor() &&
                PossibleConstructorCall->Left->AsSymbolReferenceExpression().Symbol->PProc()->GetParent()->IsStruct())
            {
                ILTree::Expression *TemporaryReference = PossibleConstructorCall->MeArgument;

                VSASSERT(TemporaryReference &&
                         TemporaryReference->bilop == SX_ADR &&
                         TemporaryReference->AsExpressionWithChildren().Left &&
                         TemporaryReference->AsExpressionWithChildren().Left->bilop == SX_SYM &&
                         TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol &&
                         TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->IsVariable(), 
                         "Unexpected valuetype instance during valuetype constructor call optimization!!!");

                if (TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->PVariable()->IsTemporary())
                {
                    // Indicate to the temp manager that this temporary is not really required.
                    m_TemporaryManager->FreeTemporary(TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->PVariable());
                }
                else
                {
                    VSASSERT(m_Procedure->IsSyntheticMethod(), "The only time a temp for struct constructor call might not be temporary is if it's already been lifted in a (synthetic) async/iterator method");                    
                }

                PossibleConstructorCall->MeArgument = MakeAddress(Target, true);  // 
                return PossibleConstructorCall;
            }
        }

        if ((Source->bilop == SX_CTYPE && Source->AsExpressionWithChildren().Left->bilop == SX_NOTHING) ||
            (Source->bilop == SX_SEQ_OP2 && Source->AsBinaryExpression().Left->bilop == SX_INIT_STRUCTURE) ||
            (Source->bilop == SX_CNS_INT && Source->vtype == t_date && Source->AsIntegralConstantExpression().Value == 0) ||
            (Source->bilop == SX_CNS_DEC && IsDecimalZeroValue(Source->AsDecimalConstantExpression())))
        {
            if (Source->bilop == SX_CTYPE && Source->AsExpressionWithChildren().Left->bilop == SX_NOTHING)
            {
                BCSYM_Variable *Temporary = AllocateDefaultValueTemporary(TargetType, &Source->Loc);

                // Note: Free this twice - once to balance the above call to allocate and the second
                // time to balance the call in Convert when this was originally created for this use.

                m_TemporaryManager->FreeTemporary(Temporary);
                m_TemporaryManager->FreeTemporary(Temporary);
            }

            else if (Source->bilop == SX_SEQ_OP2 && Source->AsBinaryExpression().Left->bilop == SX_INIT_STRUCTURE)
            {
                ILTree::Expression *TemporaryReference = Source->AsBinaryExpression().Left->AsInitStructureExpression().StructureReference;

                VSASSERT(TemporaryReference &&
                         TemporaryReference->bilop == SX_ADR &&
                         TemporaryReference->AsExpressionWithChildren().Left &&
                         TemporaryReference->AsExpressionWithChildren().Left->bilop == SX_SYM &&
                         TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol &&
                         TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->IsVariable(),
                            "Unexpected valuetype instance during valuetype constructor call optimization!!!");

                if (TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->PVariable()->IsTemporary())
                {
                    // Indicate to the temp manager that this temporary is not really required.
                    m_TemporaryManager->FreeTemporary(TemporaryReference->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->PVariable());
                }
                else
                {
                    // The above optimization is too late in the case of resumable methods (which have already had the temporary lifted)
                    VSASSERT(m_Procedure->IsSyntheticMethod(), "The only time an INITOBJ temp might not be temporary is if it's already been lifted in a (synthetic) async/iterator method");
                }
            }

            ILTree::Expression *Init =
                AllocateExpression(
                    SX_INIT_STRUCTURE,
                    TypeHelpers::GetVoidType(),
                    AssignmentLocation);

            Init->AsInitStructureExpression().StructureReference = MakeAddress(Target, true);  // 
            Init->AsInitStructureExpression().StructureType = TargetType;
            return Init;
        }
    }

    return
        AllocateExpression(
            SX_ASG,
            TypeHelpers::GetVoidType(),
            Target,
            Source,
            AssignmentLocation);
}

ILTree::Expression *
Semantics::GenerateAssignment
(
    const Location &AssignmentLocation,
    ILTree::Expression *Target,
    ILTree::Expression *Source,
    bool IsByRefCopyOut,
    bool IsAggrInitAssignment
)
{
    VSASSERT(Target->bilop != SX_SEQ_OP2 && Target->bilop != SX_ASG, "Bogus assignment target.");

    ILTree::Expression *Result =
        IsPropertyReference(Target) ?
            InterpretPropertyAssignment(
                AssignmentLocation,
                Target,
                Source,
                IsAggrInitAssignment) :
            GenerateNonPropertyAssignment(
                AssignmentLocation,
                Target,
                Source);

    if (IsByRefCopyOut && !IsBad(Result) && (Result->bilop == SX_LATE || Result->bilop == SX_VARINDEX))
    {
        SetFlag32(Result, SXF_LATE_OPTIMISTIC);
    }

    return Result;
}

ILTree::Expression *
Semantics::PassArgumentByref
(
    ILTree::Expression *Argument,
    Parameter *Param,
    Type *TargetType,
    bool CheckValidityOnly,
    bool RejectNarrowingConversions,
    bool TargetIsDllDeclare,
    ExpressionList *&CopyOutArguments,
    bool &SomeArgumentsBad,
    bool &RequiresNarrowingConversion,
    bool &RequiresSomeConversion,
    bool &AllNarrowingIsFromObject,
    bool &AllNarrowingIsFromNumericLiteral,
    bool SuppressMethodNameInErrorMessages,
    DelegateRelaxationLevel &DelegateRelaxationLevel,
    bool & RequiresUnwrappingNullable,
    bool & RequireInstanceMethodBinding
)
{
    ILTree::Expression *CopyOut = NULL;
    bool ArgumentRequiresNarrowingConversion = false;
    bool ArgumentNarrowingFromNumericLiteral = false;
    bool ArgumentRequiresCopyBackNarrowingConversion = false;
    bool ArgumentCopyBackNarrowingFromNumericLiteral = false;
    bool ArgumentRequiresUnwrappingNullable = false;
    bool ArgumentRequiresCopyBackUnwrappingNullables = false;

    Type *OriginalArgumentType = Argument->ResultType;
    ILTree::Expression *OriginalArgument = Argument;

    // DevDiv #28599 - When passing a ReadOnly structure as a ByRef inside a lambda in a
    // constructor, a compiler error should be thrown.  ReadOnly structures are not marked
    // as LValues in the constructor so normally this will result in a simulated pass by
    // ref.  Need to check directly for the compile error
    if ( m_Procedure &&
        m_Procedure->IsAnyConstructor() &&
        m_InLambda &&
        !HasFlag32(Argument, SXF_LVALUE) &&
        SX_SYM == Argument->bilop )
    {
        Variable *var = NULL;
        ILTree::SymbolReferenceExpression &symRef = Argument->AsSymbolReferenceExpression();
        if ( symRef.Symbol && symRef.Symbol->IsVariable() )
        {
            var = symRef.Symbol->PVariable();
        }

        if ( var && var->IsReadOnly() &&
                (
                    (var->IsStatic() && m_Procedure->IsSharedConstructor()) ||
                    (!var->IsStatic() && m_Procedure->IsInstanceConstructor())
                )
            )
        {
            ReportSemanticError(
                    ERRID_ReadOnlyInClosure,
                    &Argument->Loc,
                    var);
            //Passing a readonly strucutre byref inside a lambda will make the method not callable.
            //However, we do not set RequireInstanceMethodBinding to false in that case because if that
            //error is the only error that makes the method not callable then we should report that error
            //rather than trying to bind to an extension method.
            SomeArgumentsBad = true;

        }
    }

    if (HasFlag32(Argument, SXF_LVALUE) &&
        TypeHelpers::EquivalentTypes(Argument->ResultType, TargetType) &&
        !IsFieldOfMarshalByRefObject(Argument))
    {
        // Argument LValues with types that match the parameter type are
        // passed as true ByRef.

        // An argument passed to a Byref parameter must always
        // be an address.

        Argument = MakeAddress(Argument, false);

        if (IsBad(Argument))
        {
            //We failed to generate an address for some reason.
            //This should obviously mark the method as not callable.
            //However, we do not set RequireInstanceMethodBinding to false because
            //failing to generate an address is generally due to badness in the argument being passed in
            //and has nothing to do with the signature of the method. As a result, if that is the only error
            //making the method not callable (and the argumetns are all widening) then we should report that error rather than binding
            //to an extension method.
            SomeArgumentsBad = true;
        }
    }

    else if (HasFlag32(Argument, SXF_LVALUE) ||
             (IsPropertyReference(Argument) &&
              AssignmentPossible(Argument->AsPropertyReferenceExpression())))
    {
        // For a property reference passed Byref, fetch the value
        // of the property, copy the value to a temporary, pass
        // the temporary Byref, then store the value of the
        // temporary to the property after the call returns.

        // LValues with non-matching types get similar treatment.

        ILTree::Expression *ArgumentForCopyOut = NULL;
        if (!CheckValidityOnly)
        {
            UseTwiceShortLived(Argument, Argument, ArgumentForCopyOut);
        }

        Argument = MakeRValue(Argument);

        if (IsBad(Argument))
        {
            //Badness inside of UseTwiceShortLived makes the method not callable.
            //However, that badness is not caused by a property of the procedures signature
            //so we should not permit extension methods to bind
            //if that is the only error imacting callability. As a result, we set SomeArgumentsBad to true
            //but do not set RequireInstanceMethodBinding to false
            SomeArgumentsBad = true;
        }
        else
        {
            Type *SourceType = Argument->ResultType;

            Argument =
                ConvertWithErrorChecking
                (
                    Argument,
                    TargetType,
                    ExprNoFlags,
                    NULL,
                    ArgumentRequiresNarrowingConversion,
                    ArgumentNarrowingFromNumericLiteral,
                    SuppressMethodNameInErrorMessages,
                    DelegateRelaxationLevel,
                    ArgumentRequiresUnwrappingNullable
                );

            if (IsBad(Argument))
            {
                //ConvertWithErrorChecking could not convert the argument to the parameter type.
                //This makes the method not callable.
                //We set SomeArgumetnsBad to true to indicate this. We also set
                //RequireInstanceMethodBinding to false to give
                //extension methods the chance to bind.
                SomeArgumentsBad = true;
                RequireInstanceMethodBinding = false;
            }
            else if (CheckValidityOnly)
            {
                // Just make sure that the argument can be converted back to its
                // original type.

                ILTree::Expression *BogusArgument =
                    ConvertWithErrorChecking
                    (
                        Argument,
                        SourceType,
                        ExprNoFlags,
                        Param,
                        ArgumentRequiresCopyBackNarrowingConversion,
                        ArgumentCopyBackNarrowingFromNumericLiteral,
                        SuppressMethodNameInErrorMessages,
                        DelegateRelaxationLevel,
                        ArgumentRequiresCopyBackUnwrappingNullables
                    );

                if (IsBad(BogusArgument))
                {
                    //We were not able to copy back an argument passed into a procedure byref.
                    //This makes the method not callable.
                    //We set SomeArgumetnsBad to true to indicate this. We also set
                    //RequireInstanceMethodBinding to false to give
                    //extension methods the chance to bind.
                    SomeArgumentsBad = true;
                    RequireInstanceMethodBinding = false;
                }
            }
            else
            {
                Variable *CopyOutTemporary = NULL;
                Argument = CaptureInAddressedTemporary(Argument, Argument->ResultType, CopyOutTemporary);

                ILTree::Expression *ArgumentTemporaryReference =
                    AllocateSymbolReference(
                        CopyOutTemporary,
                        GetDataType(CopyOutTemporary),
                        NULL,
                        Argument->Loc);

                ArgumentTemporaryReference =
                    ConvertWithErrorChecking
                    (
                        ArgumentTemporaryReference,
                        SourceType,
                        ExprNoFlags,
                        Param,
                        ArgumentRequiresCopyBackNarrowingConversion,
                        ArgumentCopyBackNarrowingFromNumericLiteral,
                        SuppressMethodNameInErrorMessages,
                        DelegateRelaxationLevel,
                        ArgumentRequiresUnwrappingNullable
                    );

                if (IsBad(ArgumentTemporaryReference))
                {
                    //ConvertWithErrorChecking could not convert the argument to the parameter type.
                    //This makes the method not callable.
                    //We set SomeArgumetnsBad to true to indicate this. We also set
                    //RequireInstanceMethodBinding to false to give
                    //extension methods the chance to bind.
                    SomeArgumentsBad = true;
                    RequireInstanceMethodBinding = false;
                }
                else
                {
                    CopyOut =
                        GenerateAssignment(
                            ArgumentForCopyOut->Loc,
                            ArgumentForCopyOut,
                            ArgumentTemporaryReference,
                            true);

                    if (IsBad(CopyOut))
                    {
                        MakeBad(Argument);
                        CopyOut = NULL;
                        //We were not able to copy back an argument passed into a procedure byref.
                        //This makes the method not callable.
                        //We set SomeArgumetnsBad to true to indicate this. We also set
                        //RequireInstanceMethodBinding to false to give
                        //extension methods the chance to bind.
                        SomeArgumentsBad = true;
                        RequireInstanceMethodBinding = false;
                    }
                }
            }
        }
    }

    else
    {
        // Not all non-LValues are value RValues (specifically, late references
        // are not), so force the argument to be an RValue.

        Argument = MakeRValueArgument(Argument, TargetType,
                                                RequiresNarrowingConversion,
                                                AllNarrowingIsFromObject,
                                                AllNarrowingIsFromNumericLiteral
                                            );

        if (!IsBad(Argument) &&
            !TypeHelpers::EquivalentTypes(TargetType, Argument->ResultType))
        {
            RequiresSomeConversion = true;

            Argument =
                ConvertWithErrorChecking
                (
                    Argument,
                    TargetType,
                    ExprNoFlags,
                    NULL,
                    ArgumentRequiresNarrowingConversion,
                    ArgumentNarrowingFromNumericLiteral,
                    SuppressMethodNameInErrorMessages,
                    DelegateRelaxationLevel,
                    ArgumentRequiresUnwrappingNullable
                );
        }

        if (IsBad(Argument))
        {
            //ConvertWithErrorChecking could not convert the argument to the parameter type.
            //This makes the method not callable.
            //We set SomeArgumetnsBad to true to indicate this. We also set
            //RequireInstanceMethodBinding to false to give
            //extension methods the chance to bind.
            SomeArgumentsBad = true;
            RequireInstanceMethodBinding = false;
        }

        else if (!CheckValidityOnly)
        {
            Argument = CaptureInAddressedTemporary(Argument, TargetType);
        }
    }

    if (CopyOut)
    {
        CopyOutArguments =
            (ExpressionList *)AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                CopyOut,
                CopyOutArguments,
                CopyOut->Loc);
    }

    if (!IsBad(Argument) && ArgumentRequiresNarrowingConversion)
    {
        EnforceArgumentNarrowing(
            Argument,
            OriginalArgumentType,
            OriginalArgument,
            Param,
            TargetType,
            RejectNarrowingConversions,
            false,
            ArgumentNarrowingFromNumericLiteral,
            SomeArgumentsBad,
            RequiresNarrowingConversion,
            AllNarrowingIsFromObject,
            AllNarrowingIsFromNumericLiteral);
    }

    if (!IsBad(Argument) && ArgumentRequiresCopyBackNarrowingConversion)
    {
        EnforceArgumentNarrowing(
            Argument,
            TargetType,
            NULL, // Pass NULL because EnforceArgumentNarrowing() may not handle it properly for this case.
            Param,
            OriginalArgumentType,
            RejectNarrowingConversions,
            true,
            ArgumentCopyBackNarrowingFromNumericLiteral,
            SomeArgumentsBad,
            RequiresNarrowingConversion,
            AllNarrowingIsFromObject,
            AllNarrowingIsFromNumericLiteral);
    }

    if (!IsBad(Argument))
    {
        RequiresUnwrappingNullable = RequiresUnwrappingNullable || ArgumentRequiresUnwrappingNullable || ArgumentRequiresCopyBackUnwrappingNullables;
    }

    return Argument;
}

bool
Semantics::AssignmentPossible
(
    ILTree::PropertyReferenceExpression &Reference
)
{
    if (Reference.Left->bilop == SX_LATE || Reference.Left->bilop == SX_VARINDEX)
    {
        return true;
    }

    Property *ReferencedProperty = Reference.Left->AsSymbolReferenceExpression().Symbol->PProperty();

    // Find out if the property has an assignment method.

    // Doing this requires determining precisely what property to refer to, which
    // requires overload resolution.

    if (ReferencedProperty->IsOverloads())
    {
        bool SomeOperandsBad = false;
        bool ResolutionIsLateBound = false;
        bool ResolutionIsAmbiguous = false;
        GenericBinding * GenericBindingContext = NULL;

        Declaration *ResolvedProperty =
            ResolveOverloadedCall(
                Reference.Loc,
                ReferencedProperty,
                Reference.Right,
                NULL, // No delegate invoke method.
                NULL,
                GenericBindingContext,
                NULL,
                0,
                ExprNoFlags,
                OvrldNoFlags,
                InstanceTypeOfReference(Reference.Left->AsSymbolReferenceExpression().BaseReference),
                SomeOperandsBad,
                ResolutionIsLateBound,
                ResolutionIsAmbiguous);

        if (SomeOperandsBad)
        {
            return false;
        }

        if (ResolutionIsLateBound)
        {
            return true;
        }

        // Amazingly, non-properties can land here for classes written in
        // languages other than VB.

        if (!IsProperty(ResolvedProperty))
        {
            return false;
        }

        ReferencedProperty = ResolvedProperty->PProperty();
    }

    // ---- out properties without assignments.

    return MatchesPropertyRequirements(ReferencedProperty, ExprIsPropertyAssignment) != NULL;
}

ExpressionList *
Semantics::InterpretArgumentList
(
    ParseTree::ArgumentList *UnboundArguments,
    bool &SomeArgumentsBad,
    ExpressionFlags ArgumentFlags
)
{
    ExpressionList *Result = NULL;
    ExpressionList **ArgumentTarget = &Result;

    ExpressionFlags Flags = ExprPropagatePropertyReference;
    if (HasFlag(ArgumentFlags, ExprArgumentsMustBeConstant))
    {
        SetFlag(Flags, ExprMustBeConstant);
    }

    for (ParseTree::ArgumentList *Args = UnboundArguments; Args; Args = Args->Next)
    {
        ParseTree::Argument *Argument = Args->Element;
        ILTree::Expression *BoundArgument = NULL;

        if (Argument->Value)
        {
            BoundArgument = InterpretExpression(Argument->Value, Flags | ExprDontInferResultType);

            if (IsBad(BoundArgument))
            {
                SomeArgumentsBad = true;
            }
        }

        // 

        ILTree::Expression *BoundArg =
            BoundArgument ?
                AllocateExpression(
                    SX_ARG,
                    TypeHelpers::GetVoidType(),
                    BoundArgument,
                    BoundArgument->Loc) :
                NULL;

        if (Argument->Name.IsBad)
        {
            SomeArgumentsBad = true;
        }

        if (Argument->Name.Name)
        {
            SetFlag32(BoundArg, SXF_ARG_NAMED);

            ILTree::ArgumentNameExpression *ArgName = &AllocateExpression(SX_NAME, TypeHelpers::GetVoidType(), Argument->Name.TextSpan)->AsArgumentNameExpression();
            ArgName->Name = Argument->Name.Name;
            ArgName->TypeCharacter = Argument->Name.TypeCharacter;

            BoundArg->AsArgumentExpression().Name = ArgName;
        }

        ExpressionList *BoundListElement = NULL;

        BoundListElement = AllocateExpression(
            SX_LIST,
            TypeHelpers::GetVoidType(),
            BoundArg,
            NULL,
            Args->TextSpan);

        *ArgumentTarget = BoundListElement;
        ArgumentTarget = &BoundListElement->AsExpressionWithChildren().Right;
    }

    return Result;
}

bool
AllArgumentsConstant
(
    ILTree::CallExpression &LibraryCall
)
{
    for (ExpressionList *Arguments = LibraryCall.Right;
         Arguments;
         Arguments = Arguments->AsExpressionWithChildren().Right)
    {
        if (!IsConstant(Arguments->AsExpressionWithChildren().Left))
        {
            return false;
        }
    }

    return true;
}

ILTree::Expression *
Semantics::BindArgsAndInterpretCallExpressionWithNoCopyOut
(
    const Location &CallLocation,
    ILTree::Expression *Target,
    typeChars TypeCharacter,
    ParseTree::ArgumentList *UnboundArguments,
    ExpressionFlags Flags,
    OverloadResolutionFlags OvrldFlags,
    Declaration *RepresentTargetInMessages
)
{
    ExpressionList *CopyOutArguments = NULL;

    ILTree::Expression *Result =
        BindArgsAndInterpretCallExpression(
            CallLocation,
            Target,
            TypeCharacter,
            UnboundArguments,
            CopyOutArguments,
            Flags,
            OvrldFlags,
            RepresentTargetInMessages);

    if (HasFlag(Flags, ExprCreateColInitElement))
    {
        Result = AllocateColInitElement(Result, CopyOutArguments, Flags,CallLocation);
    }
    else if (!IsBad(Result))
    {
        Result = AppendCopyOutArguments(Result, CopyOutArguments, Flags);
    }

    return Result;

}

ILTree::Expression *
Semantics::BindArgsAndInterpretCallExpression
(
    const Location &CallLocation,
    ILTree::Expression *Target,
    typeChars TypeCharacter,
    ParseTree::ArgumentList *UnboundArguments,
    ExpressionList *&CopyOutArguments,
    ExpressionFlags Flags,
    OverloadResolutionFlags OvrldFlags,
    Declaration *RepresentTargetInMessages  
)
{
    // References to Me are not allowed in the arguments of a constructor call
    // if that call is the first statement in another constructor.
    bool OriginalStateValue = m_DisallowMeReferenceInConstructorCall;
    if (HasFlag(Flags, ExprIsInitializationCall))
    {
        m_DisallowMeReferenceInConstructorCall = true;
    }

    bool SomeArgumentsBad = false;
    ExpressionList *BoundArguments =
        InterpretArgumentList(
            UnboundArguments,
            SomeArgumentsBad,
            Flags & ExprArgumentsMustBeConstant);

    m_DisallowMeReferenceInConstructorCall = OriginalStateValue;

    ILTree::Expression *Result = 
        InterpretCallExpression(
            CallLocation,
            Target,
            TypeCharacter,
            BoundArguments,
            CopyOutArguments,
            SomeArgumentsBad,
            Flags,
            OvrldFlags,
            RepresentTargetInMessages);

    // Note: the callers to this function will AppendCopyOutArgumetns as necessary.
    // We also trust them to turn this SX_CALL into an SX_COLINITELEMENT if necessary.

    return Result;
}

ILTree::Expression *
Semantics::InterpretCallExpressionWithNoCopyout
(
    const Location &CallLocation,
    ILTree::Expression *Target,
    typeChars TypeCharacter,
    ExpressionList *BoundArguments,
    bool SomeArgumentsBad,
    ExpressionFlags Flags,
    Declaration *RepresentTargetInMessages
)
{
    ExpressionList *CopyOutArguments = NULL;

    ILTree::Expression *Result =
        InterpretCallExpression(
            CallLocation,
            Target,
            TypeCharacter,
            BoundArguments,
            CopyOutArguments,
            SomeArgumentsBad,
            Flags,
            OvrldNoFlags,
            RepresentTargetInMessages);

    if (HasFlag(Flags, ExprCreateColInitElement))
    {
        Result = AllocateColInitElement(Result, CopyOutArguments, Flags,CallLocation);
    }
    else if (!IsBad(Result))
    {
        Result = AppendCopyOutArguments(Result, CopyOutArguments, Flags);
    }

    return Result;
}

ILTree::Expression *
Semantics::MakeMissingArgument
(
    const Location &CallLocation
)
{
    bool ValueFieldIsBad = false;

    if (!GetFXSymbolProvider()->IsTypeAvailable(FX::MissingType))
    {
        ReportMissingType(FX::MissingType, CallLocation);
        return AllocateBadExpression(CallLocation);
    }

    Declaration *ValueField =
        EnsureNamedRoot
        (
            InterpretName
            (
                STRING_CONST(m_Compiler, Value),
                ViewAsScope(GetFXSymbolProvider()->GetType(FX::MissingType)->PContainer()),
                NULL,   // No Type parameter lookup
                NameSearchIgnoreParent | NameSearchIgnoreExtensionMethods,
                ContainingClass(),
                CallLocation,
                ValueFieldIsBad,
                NULL,   // No binding context expected
                -1
            )
        );

    if (ValueFieldIsBad)
    {
        return AllocateBadExpression(CallLocation);
    }

    if (ValueField == NULL || !ValueField->IsMember())
    {
        ReportuntimeHelperNotFoundError(
            CallLocation,
            STRING_CONST(m_Compiler, SystemReflectionMissingValue));

        return AllocateBadExpression(CallLocation);
    }

    return
        ReferToSymbol(
            CallLocation,
            ValueField,
            chType_NONE,
            NULL,
            NULL,
            ExprNoFlags);
}

ILTree::Expression *
Semantics::MakeValueTypeOrTypeParamBaseReferenceToField
(
    Declaration *ReferencedMember,
    ILTree::Expression *BaseReference,
    GenericBinding *GenericBindingContext,
    bool SuppressReadonlyLValueCapture,
    bool ConstrainValueTypeReference
)
{
    // Making a reference to a field might require a coercion. For instance, if we have "v As T" where T is
    // constrained to implement C(Of Int), and we access v.field (which is defined in C), then the CLR usually requires
    // us to DirectCast(v, C(Int)) before accessing the field.
    // We'll need the GenericBindingContext to know what exactly to cast "v" to...

    VSASSERT(!IsProcedure(ReferencedMember), "Didn't expect a procedure here");
    VSASSERT(GenericBindingContext == NULL || GenericBindingContext->IsGenericTypeBinding(), "Expected a valid generic binding context");
    return MakeValueTypeOrTypeParamBaseReferenceInternal(ReferencedMember, BaseReference, GenericBindingContext, SuppressReadonlyLValueCapture, ConstrainValueTypeReference);
}

ILTree::Expression *
Semantics::MakeValueTypeOrTypeParamBaseReferenceToProcedure
(
    Declaration *ReferencedMember,
    ILTree::Expression *BaseReference,
    bool SuppressReadonlyLValueCapture,
    bool ConstrainValueTypeReference
)
{
    // Invoking a procedure call ends up requirig coercions in fewer situations.
    // In fact, the only time when we'll have to do a DirectCast is in the case where
    // we're invoking Object/ValueType members on a struct.
    // (These won't require a GenericBindingContext. Which is lucky, because through grottiness
    // the compiler hasn't been architected to give us a GenericBindingContext in these cases).

    VSASSERT(IsProcedure(ReferencedMember), "Expected a procedure here");
    return MakeValueTypeOrTypeParamBaseReferenceInternal(ReferencedMember, BaseReference, NULL, SuppressReadonlyLValueCapture, ConstrainValueTypeReference);
}


ILTree::Expression *
Semantics::MakeValueTypeOrTypeParamBaseReferenceInternal
(
    Declaration *ReferencedMember,
    ILTree::Expression *BaseReference,
    GenericBinding *GenericBindingContext, // Only needed when !IsProcedure(ReferencedMember)
    bool SuppressReadonlyLValueCapture,
    bool ConstrainValueTypeReference
)
{
    // This routine is about satisfying the CLR's requirements for accessing procedures/fields on structs
    // or generic type parameters, e.g. "v.p()" or "v.f".
    // Sometimes we have to do it on the address of "v".
    // Sometimes we have to castclass "v" to the right thing using SX_CTYPE.
    // (Actually, the codegen phase normally ignores the ResultType of a SX_CTYPE when
    // a struct/generic is being turned into a reference type: it's just emitted as a BOX
    // and that's enough. There's only one rare case where the ResultType is important:
    // that's when we're stack-spilling a field access).

    VSASSERT(TypeHelpers::IsValueTypeOrGenericParameter(BaseReference->ResultType), "Advertised value type isn't.");

    VSASSERT(IsProcedure(ReferencedMember) ||
             !TypeHelpers::IsGenericParameter(BaseReference->ResultType) ||
             HasClassConstraint(BaseReference->ResultType->PGenericParam()),
                    "Non-proc access from non-class constrained type param unexpected!!!");

    VSASSERT(!ConstrainValueTypeReference ||
             IsProcedure(ReferencedMember),
                    "Constrained value type access for non-proc unexpected!!!");

    // For a reference to a (field or method) member of a value type, the
    // base reference is an address if the member is declared in the value
    // type, and a boxed object if the member is declared in a base class
    // (that is a reference type). Additionally for methods accessed through
    // a type param, the base reference needs to be an address.

    ILTree::Expression *Result = NULL;

    if (TypeHelpers::IsGenericParameter(BaseReference->ResultType) && IsProcedure(ReferencedMember))
    {
        // Procs in type params need to be invoked using their address, i.e.
        // as though called through a value type. They should not be boxed to
        // their constraints to make the call because that will end up creating
        // copies when the type param is a value type at runtime. Instead we
        // emit pecialized opcodes to handle this case which requires an address
        //
        Result = MakeAddress(BaseReference, SuppressReadonlyLValueCapture);
        SetFlag32(Result, SXF_CONSTRAINEDCALL_BASEREF);
    }
    else
    {
        Type *TypeDefiningMember = ReferencedMember->GetParent();

        if (TypeHelpers::IsReferenceType(TypeDefiningMember))
        {
            if (ConstrainValueTypeReference &&
                IsProcedure(ReferencedMember))
            {
                // Non-boxing proc access to interface or base method through value type.

                // Constrained base reference to value type should be the same as for unconstrained case
                // below except for the SXF_CONSTRAINEDCALL_BASEREF flag.
                //
                Result = MakeAddress(BaseReference, SuppressReadonlyLValueCapture);
                SetFlag32(Result, SXF_CONSTRAINEDCALL_BASEREF);
            }
            else
            {
                // This is a boxing conversion.
                // first check boxing of restricted types
                if (BaseReference->vtype == t_struct && m_ReportErrors)
                {
                    CheckRestrictedType(
                        ERRID_RestrictedAccess,
                        BaseReference->ResultType,
                        &BaseReference->Loc,
                        m_CompilerHost,
                        m_Errors);
                }

                if (BaseReference->vtype == t_struct &&
                    IsProcedure(ReferencedMember) &&
                    ReferencedMember->PProc()->OverriddenProc())
                {
                    Result = MakeAddress(BaseReference, SuppressReadonlyLValueCapture);
                    SetFlag32(Result, SXF_CONSTRAINEDCALL_BASEREF);
                }
                else if (IsProcedure(ReferencedMember))
                {
                    // The only way we can get in here is with "v.p()" where v is a struct, p is
                    // defined in a reference-type, and p doesn't override/implement something.
                    //
                    // Q. What does that mean? How can p() have been defined in a reference type if v is a struct?
                    // A. e.g. Dim v = 0 : v.GetType()   ' GetType was defined in a reference type
                    //
                    // Therefore p can only be one of the members defined on System.ValueType or System.Object.
                    // There's no need for any generic binding.
                    // (Which is lucky! because when we're invoked to resolve procedure members, we haven't been given one)
                    //
                    // Q. What about interface I {void p();}; struct S : I {void p() {}}; S v; v.p()
                    // A. In this case the referenced member p was defined in S which isn't a struct,
                    // so it fails the "IsReferenceType(TypeDefiningMember)" test above, and falls through
                    // to the SX_CONSTRAINEDCALL_BASEREF case at the end of this method. Similarly if S were
                    // defined in VB with explicitly-named member "p" which explicitly implements I.p
                    // (As for the case where S is defined in C# with an explicit implementation of I,
                    // then the user would have had to write CType(v,I).p(), so that BaseReference isn't
                    // any longer a value-type, and so this method wouldn't even be called!
                    //
                    // Q. What about void f<T As I>(T v) {v.p(await t);}
                    // A. In this case v was a generic type, so it was picked up by the very first If branch
                    // of this procedure and was handled with SXF_CONSTRAINEDCALL_BASEREF.

                    VSASSERT(TypeDefiningMember->GetGenericParamCount() == 0, "How are we invoking a non-virtual method on a struct which was defined in a generic type?");
                    // NB. This assert is incomplete -- should also check that TypeDefiningMember isn't nested in a generic type

                    Result =
                        AllocateExpression(
                            SX_CTYPE,
                            TypeDefiningMember,
                            BaseReference,
                            BaseReference->Loc);
                }
                else
                {
                    //plain boxing
                    BCSYM *targetType = TypeDefiningMember;
                    if (GenericBindingContext == NULL)
                    {
                        // We may be given a NULL generic binding context, in which case the above algorithm for
                        // finding TypeDefiningMember will be adequate since it'll be non-generic
                    }
                    else if (GenericBindingContext->IsGenericTypeBinding())
                    {
                        // Or we may be given a generic binding context which says how ReferencedMember was bound.
                        // This will always be a generic instantiation of the TypeDefiningMember
                        BCSYM_GenericTypeBinding *targetTypeBinding = GenericBindingContext->PGenericTypeBinding();
                        VSASSERT(TypeHelpers::EquivalentTypes(targetType, targetTypeBinding->GetGeneric()), "Expected constrained type parameter to point to concrete type in which member was defined");
                        targetType = targetTypeBinding;
                    }
                    else
                    {
                        VSFAIL("Error: don't know what generic thing to bind the constrained type parameter to");
                    }

                    Result =
                        AllocateExpression(
                            SX_CTYPE,
                            targetType,
                            BaseReference,
                            BaseReference->Loc);
                }


            }
        }
        else
        {
            // Unconstrained base reference to value type should be the same as for constrained case
            // above except for the SXF_CONSTRAINEDCALL_BASEREF flag.
            //
            Result = MakeAddress(BaseReference, SuppressReadonlyLValueCapture);
            if (BaseReference->vtype == t_struct &&
                IsProcedure(ReferencedMember) &&
                ReferencedMember->PProc()->OverriddenProc())

            {
                SetFlag32(Result, SXF_CONSTRAINEDCALL_BASEREF);
            }
        }
    }

    return Result;
}

ILTree::Expression *
Semantics::MakeCallLateBound
(
    ILTree::Expression *Target,
    Procedure *TargetProcedure,
    unsigned TypeArgumentCount,
    Type **TypeArguments,
    Location *TypeArgumentLocations,
    GenericBinding *TargetBinding,
    ExpressionList *BoundArguments,
    const Location &CallLocation,
    ExpressionFlags Flags
)
{
    if (Target)
    {
        Target = Convert(MakeRValue(Target), GetFXSymbolProvider()->GetObjectType(), ExprNoFlags, ConversionWidening);
    }

    ILTree::Expression *Late =
        AllocateExpression(
            SX_LATE,
            GetFXSymbolProvider()->GetObjectType(),
            Target,
            NULL,
            CallLocation);
    Late->AsLateBoundExpression().LateIdentifier = ProduceStringConstantExpression(
        TargetProcedure->GetName(),
        wcslen(TargetProcedure->GetName()),
        CallLocation
        IDE_ARG(0));

    if (TypeArgumentCount > 0)
    {
        // Interpret the type arguments.

        if (!GetFXSymbolProvider()->IsTypeAvailable(FX::TypeType))
        {
            ReportMissingType(FX::TypeType, CallLocation);
            return AllocateBadExpression(CallLocation);
        }

        ExpressionList *TypeArgumentsList = NULL;
        ExpressionList **TargetExpressionList = &TypeArgumentsList;
        unsigned TypeArgumentIndex = 0;

        for (TypeArgumentIndex = 0; TypeArgumentIndex < TypeArgumentCount; TypeArgumentIndex++)
        {
            if (m_ReportErrors)
            {
                // Check for restricted types in type arguments passed to late bound expression.
                // 

                CheckRestrictedType(
                    ERRID_RestrictedType1,
                    TypeArguments[TypeArgumentIndex],
                    &TypeArgumentLocations[TypeArgumentIndex],
                    m_CompilerHost,
                    m_Errors);
            }

            *TargetExpressionList =
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    AllocateExpression(
                        SX_METATYPE,
                        GetFXSymbolProvider()->GetTypeType(),
                        AllocateExpression(
                            SX_NOTHING,
                            TypeArguments[TypeArgumentIndex],
                            TypeArgumentLocations[TypeArgumentIndex]),
                        CallLocation),
                    CallLocation);

            TargetExpressionList = &(*TargetExpressionList)->AsExpressionWithChildren().Right;
        }

        ArrayType *TypeArgumentArrayType = m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetTypeType());

        Late->AsLateBoundExpression().TypeArguments =
            InitializeArray(
                TypeArgumentsList,
                TypeArgumentArrayType,
                NULL,
                CallLocation);
    }

    if (Target == NULL)
    {
        Late->AsLateBoundExpression().LateClass =
            TargetBinding ? TargetBinding : TargetProcedure->GetParent();
    }

    Late =
        AllocateExpression(
            SX_LATE_REFERENCE,
            GetFXSymbolProvider()->GetObjectType(),
            Late,
            NULL,
            CallLocation);


    return
        InterpretLateBoundExpression(
            CallLocation,
            Late->AsPropertyReferenceExpression(),
            BoundArguments,
            Flags);
}


//Reutrns true if LookupResult contains an instance method result
//that has at least one accessible overload that is callable with zero arguments.
bool
Semantics::HasAccessibleZeroArgumentInstanceMethods
(
    ExtensionCallLookupResult * LookupResult
)
{
    Declaration * pDecl = LookupResult->GetInstanceMethodLookupResult();

    while (pDecl)
    {
        Procedure * pProc = ViewAsProcedure(pDecl);

        if (!pProc->GetFirstParam() || pProc->GetFirstParam()->IsOptional() || pProc->GetFirstParam()->IsParamArray())
        {
            return true;
        }
        else
        {
            pDecl =
                GetNextOverloadForProcedureConsideringBaseClasses
                (
                    pDecl,
                    LookupResult->GetAccessingInstanceTypeOfInstanceMethodLookupResult()
                );
        }
    }

    return false;
}

//Notice that this function does not free memory, but simply
//invokes a destructor on the pointer it's given. This is
//because this function is designed to run on data
//that has been allocated on the NorlsAllocator. It primary purpose
//is to be used as a "destruction hook" that will be run when
//a norls allocator region containing pv is either rolled back or destroyed.
void Semantics::DestroyErrorTable(void * pv)
{
    //Invoke the error table destructor on the object.
    ((ErrorTable *)(pv))->~ErrorTable();
}

ILTree::Expression *
Semantics::ReferToExtensionMethod
(
    const Location &ReferringLocation,
    ExtensionCallLookupResult * LookupResult,
    ILTree::Expression * BaseReference,
    ExpressionFlags Flags,
    typeChars TypeCharacter
)
{

    if (Flags & ExprMustBeConstant)
    {
        ReportSemanticError(ERRID_RequiredConstExpr, ReferringLocation);
        return AllocateBadExpression(ReferringLocation);
    }

    //We are trying to refer to an extension call lookup result.
    //There are 2 cases where we would do this: One where we
    //want to generate a call expression and the other where
    //we want to generate an SX_EXTENSION_CALL expression.
    //We want to generate an SX_CALL
    //in cases where the method is reffered to in the form of "a.b" or "b". In the case
    //where a real call happens (a.b(...), b(of ...)(...)), etc, we want to generate an
    //SX_EXTENSION_CALL and then do overload resolution later.
    //The behavior is controlled by the flags we are passed in.
    //In particular, when (Flags & (ExprIsExplicitCallTarget | ExprSuppressDefaultInstanceSynthesis)) == ExprIsExplicitCallTarget
    //we want to generate an SX_EXTENSION_CALL. In all other cases we want to generate
    //an SX_CALL. See the comments on the definition of ExprSuppressDefaultInstanceSynthesis for more details...
    //
    ThrowIfNull(LookupResult);
    unsigned errorid = 0;

    bool synthesizedMeReference = false;

    if (! BaseReference)
    {
        bool reportError =
            !LookupResult->GetInstanceMethodLookupResult() ||
            !HasAccessibleSharedOverload
            (
                LookupResult->GetInstanceMethodLookupResult(),
                LookupResult->GetInstanceMethodLookupGenericBinding(),
                LookupResult->GetAccessingInstanceTypeOfInstanceMethodLookupResult()
            );

        BaseReference = SynthesizeMeReference(ReferringLocation, ContainingClass(), Flags & ExprSuppressMeSynthesis, reportError, reportError ? NULL : &errorid);
        synthesizedMeReference = true;
    }

    ThrowIfNull(BaseReference);
    ThrowIfNull(m_CompilerHost);

    ILTree::ExtensionCallExpression * extCall =
        AllocateExtensionCall
        (
            BaseReference,
            LookupResult,
            ReferringLocation,
            errorid,
            synthesizedMeReference
        );


    return ReferToExtensionMethod(ReferringLocation, extCall, Flags, TypeCharacter);
}

ILTree::Expression *
Semantics::ReferToExtensionMethod
(
    const Location &ReferringLocation,
    ILTree::ExtensionCallExpression * ExtensionCall,
    ExpressionFlags Flags,
    typeChars TypeCharacter
)
{
    ThrowIfNull(ExtensionCall);

    if ((Flags & (ExprIsExplicitCallTarget | ExprSuppressDefaultInstanceSynthesis)) == ExprIsExplicitCallTarget)
    {
        return ExtensionCall;
    }
    else
    {
        return
            BindArgsAndInterpretCallExpressionWithNoCopyOut
            (
                ReferringLocation,
                ExtensionCall,
                TypeCharacter,
                NULL,
                Flags,
                OvrldNoFlags,
                NULL
            );
    }
}

ILTree::Expression *
Semantics::ReferToProcByName
(
    const Location &ReferringLocation,
    Container *Container,
    _In_ STRING *ProcName,
    ILTree::Expression *BaseReference,
    ExpressionFlags Flags
)
{
    if (Container)
    {
        Symbol *Referenced = Container->GetHash()->SimpleBind(ProcName);
        if (Referenced)
        {
            return ReferToSymbol(
                ReferringLocation,
                Referenced,
                chType_NONE,
                BaseReference,
                NULL,
                Flags | ExprIsExplicitCallTarget);
        }
    }
    return NULL;
}


ILTree::Expression *
Semantics::InterpretExtensionCallExpression
(
    const Location &CallLocation,
    ILTree::ExtensionCallExpression * extCall,
    typeChars TypeCharacter,
    ExpressionList *BoundArguments,
    ExpressionList *&CopyOutArguments,
    bool SomeArgumentsBad,
    ExpressionFlags Flags,
    OverloadResolutionFlags OvrldFlags,
    Declaration *RepresentTargetInMessages
)
{
    ExpressionListHelper listHelper(this, extCall->ImplicitArgumentList);
    listHelper.Splice(BoundArguments);
    unsigned int BoundArgumentCount = listHelper.Count();
    BoundArguments = listHelper.Start();

    ILTree::Expression * MethodCall = NULL;
    bool ResultIsExtensionMethod = false;

    if (!SomeArgumentsBad)
    {
        MethodCall =
            ResolveExtensionCallOverloadingAndReferToResult
            (
                extCall,
                BoundArguments,
                BoundArgumentCount,
                Flags,
                OvrldFlags,
                CallLocation,
                ResultIsExtensionMethod
            );
    }

    ILTree::Expression * pRet = NULL;
    if (MethodCall && !IsBad(MethodCall))
    {
        if (!ResultIsExtensionMethod)
        {
            //We bound to an instance method inside overload resolution
            //As a result we no longer want to pass in the receiver
            //as an argument.
            //The call to ResolveExtensionCallOverloadingAndReferToResult
            //took care of converting the receiver into an RValue
            //(thus flatening property references) and setting it up as the base reference of
            //the result. Our job here is just to "remove" the receiver from the list
            //of argumetns that pass off to InterpretCallExpression.
            BoundArguments = BoundArguments->AsExpressionWithChildren().Right;
        }

        pRet =
            InterpretCallExpression
            (
                CallLocation,
                MethodCall,
                TypeCharacter,
                BoundArguments,
                CopyOutArguments,
                SomeArgumentsBad,
                Flags | ExprSkipOverloadResolution,
                //We resolved extension method overload resolution and are
                //now generate a method call. In this case we
                //don't want to get any more extension method
                //semantics so we remove OvrldsomeCandidatesAreExtensionMethods
                //from our overload flags.
                OvrldFlags & ~(OvrldSomeCandidatesAreExtensionMethods),
                RepresentTargetInMessages
            );

    }
    else
    {
        //Overload resolution failed.
        //We set ResultIsExtensionMethod to true so that we
        //will set the value of SXF_CALL_WAS_EXTENSION_CALL
        //below.
        ResultIsExtensionMethod = true;
        pRet =
        MakeBad
        (
            AllocateExpression
            (
                SX_CALL,
                TypeHelpers::GetVoidType(),
                MakeBad
                (
                    AllocateSymbolReference
                    (
                        extCall->ExtensionCallLookupResult->GetFirstExtensionMethod(),
                        TypeHelpers::GetVoidType(),
                        NULL,
                        CallLocation
                    )
                ),
                BoundArguments,
                CallLocation
            )
        );

    }

    if (ResultIsExtensionMethod)
    {
        SetFlag32(pRet, SXF_CALL_WAS_EXTENSION_CALL);
    }
    return pRet;
}

ILTree::Expression *
Semantics::InterpretCallExpression
(
    const Location &CallLocation,
    ILTree::Expression *Target,
    typeChars TypeCharacter,
    ExpressionList *BoundArguments,
    ExpressionList *&CopyOutArguments,
    bool SomeArgumentsBad,
    ExpressionFlags Flags,
    OverloadResolutionFlags OvrldFlags,
    Declaration *RepresentTargetInMessages
)
{
    
    VSASSERT(CopyOutArguments == NULL, "Trash in ByRef argument copy list.");

    // If a reference to a procedure is through an alias, the symbol that
    // appears as the target of the call is the alias symbol. Therefore, the
    // compiler must be careful to keep track of the alias symbol as well as
    // the procedure symbol.

    Procedure *TargetProcedure = NULL;
    Declaration *TargetDeclaration = NULL;

    Type **TypeArguments = NULL;
    Location *TypeArgumentLocations = NULL;
    unsigned TypeArgumentCount = 0;
    GenericBinding *GenericBindingContext = NULL;

    bool SomeOperandsBad = SomeArgumentsBad || IsBad(Target);
    bool OperandArityBad = false;

    if (!IsBad(Target))
    {
        if (Target->bilop == SX_SYM && IsProcedure(Target->AsSymbolReferenceExpression().Symbol))
        {
            TargetDeclaration = Target->AsSymbolReferenceExpression().Symbol;
            TargetProcedure = ViewAsProcedure(TargetDeclaration);
            GenericBindingContext = Target->AsSymbolReferenceExpression().GenericBindingContext;

            if (IsProperty(TargetProcedure))
            {
                // Parameters to properties are necessarily ByVal. In order for
                // multiple uses of the argument list (e.g. if the property
                // ends up being passed ByRef and so both a Get and Set are
                // necessary) to correctly capture the arguments, turn all
                // arguments into RValues.

                // Parameters of properties imported from COM can by ByRef, but
                // we want to preserve ByVal semantics.

                if (!SomeArgumentsBad)
                {
                    for (ExpressionList *Args = BoundArguments; Args; Args = Args->AsExpressionWithChildren().Right)
                    {
                        ILTree::Expression *Argument = Args->AsExpressionWithChildren().Left;
                        if (Argument)
                        {
                            VSASSERT(Argument->bilop == SX_ARG, "Malformed argument list.");

                            ILTree::Expression *ArgumentValue = Argument->AsArgumentExpression().Left;

                            ArgumentValue = MakeRValue(ArgumentValue);

                            if (IsBad(ArgumentValue))
                            {
                                SomeArgumentsBad = true;
                            }

                            Argument->AsArgumentExpression().Left = ArgumentValue;
                        }
                    }
                }

                if (HasFlag(Flags, ExprResultNotNeeded) && !HasFlag(Flags, ExprIsPropertyAssignment))
                {
                    ReportSemanticError(
                        ERRID_PropertyAccessIgnored,
                        CallLocation);

                    SomeOperandsBad = true;
                }

                if (HasFlag(Flags, ExprPropagatePropertyReference))
                {
                    // If a reference to a property occurs as the target of an
                    // assignment, the property method invocation can't be
                    // processed until the source value is available. If a property
                    // reference occurs as an operand to an operator assignment
                    // (e.g. +=) or as a byref argument, both a fetch from and a
                    // store to the property will be necessary. Therefore, it isn't
                    // practical to fully interpret a property reference here.

                    // References to properties get processed when the full context
                    // is available. as part of the assignment, because that's the
                    // point where all the arguments are known.

                    Type *ResultType = GetReturnType(TargetProcedure);
                    if (GenericBindingContext)
                    {
                        ResultType = ReplaceGenericParametersWithArguments(ResultType, GenericBindingContext, m_SymbolCreator);
                    }

                    ILTree::Expression *Result =
                        AllocateExpression(
                            SX_PROPERTY_REFERENCE,
                            ResultType,
                            Target,
                            BoundArguments,
                            CallLocation);
                    Result->AsPropertyReferenceExpression().TypeCharacter = TypeCharacter;

                    if (SomeOperandsBad)
                    {
                        MakeBad(Result);
                    }

                    if (HasFlag(Flags, ExprMustBeConstant))
                    {
                        ReportSemanticError(
                            ERRID_RequiredConstExpr,
                            CallLocation);

                        MakeBad(Result);
                    }

                    // Check for implicit use of Me.  Property Reference is a special case because
                    // synthesizing Me doesn't happen until later.

                    if (m_DisallowMeReferenceInConstructorCall &&
                        !Target->AsSymbolReferenceExpression().BaseReference &&
                        !TargetProcedure->IsShared())
                    {
                        ReportSemanticError(
                            ERRID_InvalidImplicitMeReference,
                            Target->Loc);
                        // Don't mark this expression bad so we can continue analyzing it.
                    }

                    return Result;
                }
            }
        }
        else if (TypeHelpers::IsDelegateType(Target->ResultType))
        {
            // An invocation of a delegate actually calls the delegate's
            // Invoke method.

            bool InvokeIsBad = false;

            //We don't actually need to pass in
            //ignore extension methods here, because
            //delegate types will have a DelegateInvoke method
            //which we will end up binding to before extension methods.
            //However, we pass it in anyways, just to be safe.
            Declaration *Invoke =
                EnsureNamedRoot
                (
                    InterpretName
                    (
                        STRING_CONST(m_Compiler, DelegateInvoke),
                        ViewAsScope(Target->ResultType->PClass()),
                        NULL,   // No Type parameter lookup
                        NameSearchIgnoreParent | NameSearchIgnoreExtensionMethods,
                        ContainingClass(),
                        Target->Loc,
                        InvokeIsBad,
                        NULL,   // Binding context got later below
                        -1
                    )
                );

            if ((Invoke == NULL || !IsProcedure(Invoke)) &&
                !InvokeIsBad)
            {
                ReportSemanticError(
                    ERRID_DelegateNoInvoke1,
                    Target->Loc,
                    Target->ResultType);

                InvokeIsBad = true;
            }

            if (InvokeIsBad)
            {
                MakeBad(Target);
                SomeOperandsBad = true;
            }
            else
            {
                ILTree::Expression* Result =
                    InterpretCallExpression(
                        CallLocation,
                        ReferToSymbol(
                            Target->Loc,
                            Invoke,
                            chType_NONE,
                            Target,
                            DeriveGenericBindingForMemberReference(Target->ResultType, Invoke, m_SymbolCreator, m_CompilerHost),
                            ExprIsExplicitCallTarget),
                        TypeCharacter,
                        BoundArguments,
                        CopyOutArguments,
                        SomeArgumentsBad,
                        Flags,
                        OvrldFlags,
                        Invoke->GetParent());
                SetFlag32(Result, SXF_CALL_WAS_IMPLICIT_INVOKE);
                return Result;
            }
        }
        else if (Target->bilop == SX_OVERLOADED_GENERIC)
        {
            TargetDeclaration = Target->AsOverloadedGenericExpression().BaseReference->AsSymbolReferenceExpression().Symbol;
            TargetProcedure = ViewAsProcedure(TargetDeclaration);

            TypeArguments = Target->AsOverloadedGenericExpression().TypeArguments;
            TypeArgumentLocations = Target->AsOverloadedGenericExpression().TypeArgumentLocations;
            TypeArgumentCount = Target->AsOverloadedGenericExpression().TypeArgumentCount;

            Target = Target->AsOverloadedGenericExpression().BaseReference;
        }
        else if (Target->bilop == SX_EXTENSION_CALL)
        {

            ILTree::ExtensionCallExpression * pExtensionCall = &Target->AsExtensionCallExpression();

            return
                InterpretExtensionCallExpression
                (
                    CallLocation,
                    pExtensionCall,
                    TypeCharacter,
                    BoundArguments,
                    CopyOutArguments,
                    SomeArgumentsBad,
                    Flags,
                    OvrldFlags | OvrldSomeCandidatesAreExtensionMethods,
                    RepresentTargetInMessages
                );
        }
        else
        {
            ReportSemanticError(
                ERRID_ExpectedProcedure,
                Target->Loc);

            MakeBad(Target);
            SomeOperandsBad = true;
        }
    }

    bool AlteredTargetProcedure = false;
    GenericBinding *TargetBinding =
        Target->bilop == SX_SYM ?
            Target->AsSymbolReferenceExpression().GenericBindingContext :
            NULL;

    if
    (
        !SomeOperandsBad &&
        (
            TargetProcedure->IsOverloads() ||
            (
                IsGeneric(TargetProcedure) &&
                (TargetBinding == NULL || TargetBinding->GetGeneric() != TargetProcedure)
            ) ||
            (OvrldFlags & OvrldForceOverloadResolution)
        ) &&
        ! (Flags & ExprSkipOverloadResolution)
    )
    {
        bool ResolutionIsLateBound = false;
        bool ResolutionIsAmbiguous= false;
        GenericBindingContext = TargetBinding;

        // We do not want to report any lambda type inference errors during overload resolution. We will report the neccessary errors
        // when we interpret the lambda that is the correct match. See InferMultilineLambdaReturnTypeFromReturnStatements for more details.
        BackupValue<TriState<bool>> backupReportLambdaTypeInferenceErrors(&m_ReportMultilineLambdaReturnTypeInferenceErrors);
        m_ReportMultilineLambdaReturnTypeInferenceErrors.SetValue(false);

        if (OvrldFlags & OvrldSkipTargetResolution)
        {
            // This means that someone gave us a TargetDeclaration that was already resolved:
            // there's no need to do resolution again.
            // (Indeed, they might even have provided a GenericBindingContext that's for the
            // resolved method, rather than a GenericTypeBindingContext as required by the
            // following method).
        }
        else
        {
            TargetDeclaration =
                ResolveOverloadedCall(
                    CallLocation,
                    TargetDeclaration,
                    BoundArguments,
                    NULL, // No delegate invoke method
                    NULL,
                    GenericBindingContext, // ByRef
                    TypeArguments,
                    TypeArgumentCount,
                    Flags,
                    OvrldFlags,
                    InstanceTypeOfReference(Target->AsSymbolReferenceExpression().BaseReference),
                    SomeOperandsBad,
                    ResolutionIsLateBound,
                    ResolutionIsAmbiguous
                );
        }

        if (ResolutionIsLateBound)
        {
            VSASSERT(!SomeOperandsBad, "expected no bad operands for latebound overload resolution");

            ILTree::Expression *BaseReference = Target->AsSymbolReferenceExpression().BaseReference;

            if (BaseReference == NULL)
            {
                if (m_Procedure &&
                    !m_Procedure->IsShared() &&                             // - in shared methods, don't try to use Me. Try to bind among only the shared methods because Me cannot be loaded
                                                                            //      This although different from early bound behavior is consistent with today's latebound behavior. Separate 
                    !m_DisallowMeReferenceInConstructorCall &&              // - cannot use instance methods in Mybase.New. so should we let it bind to shared methods only ? - seems wrong ?
                                                                            //      but for now to be consistent with the rest of our late bound behavior where in shared contexts,
                                                                            //      we consider only the shared methods when doing overload resolution. This is not consistent with early bound,
                                                                            //      but there is a separate 
                    !HasFlag(Flags, (unsigned)ExprSuppressMeSynthesis) &&    // - verify that if using TypeName.SharedMethod syntax. In this case, we don't want to use Me, but only bind to shared methods
                    IsOrInheritsFrom(ContainingClass(), TargetProcedure->GetParent())) // - verify that this method belongs to the current class or one of its bases. We should not do this this Foo() where Foo is from another type like a Module
                {
                    VSASSERT(
                        m_Procedure->GetParent() == ContainingClass(),
                        "How can current type context not be the parent of the current procedure?");

                    BaseReference = AllocateSymbolReference(ContainingClass()->GetMe(), ContainingClass(), NULL, Target->Loc);
                }
            }

            return MakeCallLateBound(
                BaseReference,
                TargetProcedure,
                TypeArgumentCount,
                TypeArguments,
                TypeArgumentLocations,
                TargetBinding,
                BoundArguments,
                CallLocation,
                Flags);
        }

        TargetProcedure = SomeOperandsBad ? NULL : ViewAsProcedure(TargetDeclaration);
        AlteredTargetProcedure = true;
    }


    if (!SomeOperandsBad && IsProperty(TargetProcedure) && TargetProcedure->IsOverrides())
    {
        TargetProcedure =
            ResolveOverriddenProperty(
                TargetProcedure,
                Flags,
                AlteredTargetProcedure);
    }

    if (!SomeOperandsBad && AlteredTargetProcedure)
    {
        // If the bound member is overloaded, then we need to make a special check here for
        // obsoleteness and availability because the normal channels through CheckAcessibility
        // have been averted.

        CheckObsolete(TargetProcedure, CallLocation);

        if (!DeclarationIsAvailableToCurrentProject(TargetProcedure))
        {
            if (m_ReportErrors)
            {
                StringBuffer TextBuffer1;

                ReportSmartReferenceError(
                    ERRID_SymbolFromUnreferencedProject3,
                    m_Project,
                    TargetProcedure->GetContainingProject(),
                    m_Compiler,
                    m_Errors,
                    TargetProcedure->GetContainingProject()->GetFileName(),     // 
                    &CallLocation,
                    ExtractErrorName(TargetProcedure, TextBuffer1),
                    GetErrorProjectName(TargetProcedure->GetContainingProject()),
                    GetErrorProjectName(m_Project));
            }

            return AllocateBadExpression(CallLocation);
        }

        TargetBinding = Target->AsSymbolReferenceExpression().GenericBindingContext;

        if (OvrldFlags & OvrldSkipTargetResolution)
        {
            // This means that someone gave us a TargetDeclaration that was already resolved:
            // there's no need to do resolution again
            // (Indeed, they might even have provided a GenericBindingContext that's for the
            // resolved method, rather than a GenericTypeBindingContext as required by the
            // following method).
        }
        else
        {
            TargetBinding =
                DeriveGenericBindingForMemberReference(
                    TargetBinding ?
                        TargetBinding :
                        Target->AsSymbolReferenceExpression().pnamed->GetContainer()->PNamedRoot(),
                    TargetProcedure,
                    m_SymbolCreator,
                    m_CompilerHost);
        }

        // If type arguments were supplied, create the generic binding now that the target is known.

        if (TypeArgumentCount > 0)
        {
            TargetBinding =
                ValidateGenericArguments(
                    CallLocation,
                    TargetProcedure,
                    TypeArguments,
                    TypeArgumentLocations,
                    TypeArgumentCount,
                    TargetBinding->PGenericTypeBinding(),
                    SomeOperandsBad);
        }

        Target->AsSymbolReferenceExpression().GenericBindingContext = TargetBinding;

        // 


        if (m_IsGeneratingXML)
        {
            // The generic arguments to include appear to the right of the call symbol
            // This should not be done for properties.
            if (CallLocation.EndsAfter(&Target->AsSymbolReferenceExpression().Loc) && !IsProperty(TargetProcedure))
            {
                Target->AsSymbolReferenceExpression().Loc.m_lEndLine = CallLocation.m_lEndLine;
                Target->AsSymbolReferenceExpression().Loc.m_lEndColumn = CallLocation.m_lEndColumn;
            }
        }
    }

    // Update the bound tree to refer to the resolved procedure. However, if the
    // resolution fails, leave the bound tree referring to the overload. (This is
    // conceivably of some use to Intellisense.)

    if (!SomeOperandsBad)
    {
        if (IsProperty(TargetProcedure))
        {
            TargetDeclaration = MatchesPropertyRequirements(TargetProcedure, Flags);

            if (!TargetDeclaration)
            {
                ReportPropertyMismatch(TargetProcedure, Flags, CallLocation);
                SomeOperandsBad = true;
                TargetProcedure = NULL;
            }
            else if (!IsAccessible(
                        TargetDeclaration,
                        Target->AsSymbolReferenceExpression().GenericBindingContext,
                        InstanceTypeOfReference(Target->AsSymbolReferenceExpression().BaseReference)))
            {
                // the property might be accessible but the accessor we link the call for can have a more restricive
                // accessibility
                ReportSemanticError(
                    HasFlag(Flags, ExprIsPropertyAssignment) ? ERRID_NoAccessibleSet : ERRID_NoAccessibleGet,
                    CallLocation,
                    TargetProcedure->GetErrorName(m_Compiler));

                SomeOperandsBad = true;
                TargetProcedure = NULL;
            }
            else if (TargetDeclaration == m_Procedure &&
                     TargetProcedure->PProperty()->GetParameterCount() < 1 &&
                     ( TargetProcedure->IsShared() ||
                        (Target->AsSymbolReferenceExpression().BaseReference == 0 ||
                         Target->AsSymbolReferenceExpression().BaseReference->bilop == SX_SYM &&
                         Target->AsSymbolReferenceExpression().BaseReference->AsSymbolReferenceExpression().Symbol->IsVariable() &&
                         Target->AsSymbolReferenceExpression().BaseReference->AsSymbolReferenceExpression().Symbol->PVariable()->IsMe())))
            {
                ReportSemanticError(
                    WRNID_RecursivePropertyCall,
                    CallLocation,
                    TargetProcedure->GetErrorName(m_Compiler));
            }
        }
        else if (IsEvent(TargetProcedure))
        {
            ReportSemanticError(
                ERRID_CannotCallEvent1,
                Target->Loc,
                TargetProcedure);

            SomeOperandsBad = true;
            TargetProcedure = NULL;
        }
        if (!SomeOperandsBad)
        {
            // Fix for Whidbey - 32816
            // For properties, check the obsoleteness of the individual
            // getter or setter that is actually being bound to based on
            // the context.
            //
            if (IsProperty(TargetProcedure))
            {
                CheckObsolete(TargetDeclaration, CallLocation);
            }

            Target->AsSymbolReferenceExpression().Symbol = TargetDeclaration;
            TargetProcedure = ViewAsProcedure(TargetDeclaration);
            LogDependency(TargetDeclaration);
        }
    }

    // Deal with the instance argument.

    ILTree::Expression *MeArgument = NULL;

    if (!SomeOperandsBad)
    {
        // The Me argument does not occur in the output trees as the
        // base reference of the the target, so fetch it.

        MeArgument = Target->AsSymbolReferenceExpression().BaseReference;
        Target->AsSymbolReferenceExpression().BaseReference = NULL;

        if (m_CallGraph)
        {
            // Add the call to the graph if it has an instance method has an explicit Me.
            if (MeArgument &&
                MeArgument->bilop == SX_SYM &&
                MeArgument->AsSymbolReferenceExpression().Symbol->PVariable()->IsMe() &&
                !TargetProcedure->IsShared())
            {
                m_CallGraph->AddCall(TargetProcedure);
            }
        }

        if (TargetProcedure->IsShared())
        {
            if (HasFlag(Flags, ExprIsLHSOfObjectInitializer))
            {
                AssertIfFalse(TargetProcedure->IsPropertySet());

                ReportSemanticError(
                    ERRID_SharedMemberAggrMemberInit1,
                    Target->Loc,
                    TargetProcedure->IsPropertySet() ?
                        TargetProcedure->GetAssociatedPropertyDef()->GetName() :
                        TargetProcedure->GetName());

                SomeOperandsBad = true;
                TargetProcedure = NULL;
            }
            else if (MeArgument &&
                     !MeArgument->NameCanBeType &&
                     m_Procedure &&
                     // Dev10 426874
                     // We want to generate the warning even for synthetic methods that are
                     // in the constructor.
                     (!m_Procedure->IsSyntheticMethod() ||
                      m_Procedure->PSyntheticMethod()->GetSyntheticKind() == SYNTH_New ||
                      m_Procedure->PSyntheticMethod()->GetSyntheticKind() == SYNTH_SharedNew))
                     //!m_Procedure->IsSyntheticMethod())
            {
                ReportSemanticError(
                    WRNID_SharedMemberThroughInstance,
                    Target->Loc);
            }

            // The extraneous Me argument is not evaluated.
            if (!m_IsGeneratingXML)
            {
                MeArgument = NULL;
            }
        }
        else if (MeArgument == NULL &&
                 // Constructor calls that occur in New expessions
                 // don't have Me arguments. Other valid constructor
                 // calls supply an object explicitly.
                 !TargetProcedure->IsInstanceConstructor())
        {
            MeArgument =
                SynthesizeMeReference(
                    Target->Loc,
                    TargetProcedure->GetParent(),
                    HasFlag32(Target, SXF_SYM_MAKENOBASE));

            if (IsBad(MeArgument))
            {
                SomeOperandsBad = true;
            }
            else
            {
                // Add this call to the graph because me was synthesized.
                if (m_CallGraph)
                {
                    m_CallGraph->AddCall(TargetProcedure);
                }
            }
        }
    }

    // The type of a function symbol is the return type, and is
    // null (not void) for a Sub.

    Type *ResultType =
        (TargetProcedure && TargetProcedure->GetType()) ?
            GetReturnType(TargetProcedure) :
            TypeHelpers::GetVoidType();

    if (TypeHelpers::IsBadType(ResultType))
    {
        ReportBadType(ResultType, CallLocation);
        SomeOperandsBad = true;
    }

    if (TargetProcedure)
    {
        if (!SomeOperandsBad)
        {
            if (IsPropertyGet(TargetProcedure) && HasFlag(Flags, ExprIsPropertyAssignment))
            {
                ReportSemanticError(
                    ERRID_ReadOnlyProperty1,
                    Target->Loc,
                    // Use only the property's name, not its full description.
                    TargetProcedure->GetErrorName(m_Compiler));

                SomeOperandsBad = true;
            }
            else
            {
                GenericBindingContext =
                    Target->bilop == SX_SYM ? Target->AsSymbolReferenceExpression().GenericBindingContext : NULL;


                bool RequiresNarrowingConversion = false;
                bool RequiresSomeConversion = false;
                bool AllNarrowingIsFromObject = true;
                bool AllNarrowingIsFromNumericLiteral = true;
                bool InferenceFailed = false;
                bool AllFailedInferenceIsDueToObject = true;
                DelegateRelaxationLevel DelegateRelaxationLevel = DelegateRelaxationLevelNone;
                TypeInferenceLevel TypeInferenceLevel = TypeInferenceLevelNone;
                bool RequiresUnwrappingNullable = false;
                bool RequiresInstanceMethodBinding = false;
                Location *pCallerInfoLineNumber = Target != NULL ? &(Target->Loc) : NULL;

                BoundArguments =
                    MatchArguments1
                    (
                        CallLocation,
                        TargetProcedure,
                        RepresentTargetInMessages,
                        GenericBindingContext,
                        BoundArguments,
                        NULL,
                        Flags,
                        OvrldFlags,
                        CopyOutArguments,
                        false,
                        false,
                        false,
                        false,
                        SomeOperandsBad,
                        OperandArityBad,
                        RequiresNarrowingConversion,
                        RequiresSomeConversion,
                        AllNarrowingIsFromObject,
                        AllNarrowingIsFromNumericLiteral,
                        InferenceFailed,
                        AllFailedInferenceIsDueToObject,
                        false,
                        false,
                        NULL,
                        DelegateRelaxationLevel,
                        TypeInferenceLevel,
                        RequiresUnwrappingNullable,
                        RequiresInstanceMethodBinding,
                        pCallerInfoLineNumber
                    );

                if (Target->bilop == SX_SYM)
                {
                    Target->AsSymbolReferenceExpression().GenericBindingContext = GenericBindingContext;
                }
            }
        }
    }

    if (Target->bilop == SX_SYM && Target->AsSymbolReferenceExpression().GenericBindingContext)
    {
        ResultType = ReplaceGenericParametersWithArguments(ResultType, Target->AsSymbolReferenceExpression().GenericBindingContext, m_SymbolCreator);
    }

    if (TargetProcedure)
    {
        // Type character validation

        // If a type character is involved, it generally refers to the return
        // type of the procedure. However, for property assignments, it refers
        // to the type of the parameter that receives the property value.
        //
        Type *TypeCharacterVerificationType = NULL;
        if (IsPropertySet(TargetProcedure))
        {
            if (Target->bilop == SX_SYM && Target->AsSymbolReferenceExpression().GenericBindingContext)
            {
                TypeCharacterVerificationType =
                    ReplaceGenericParametersWithArguments(
                        GetDataType(TargetProcedure->GetLastParam()),
                        Target->AsSymbolReferenceExpression().GenericBindingContext,
                        m_SymbolCreator);
            }
            else
            {
                TypeCharacterVerificationType = GetDataType(TargetProcedure->GetLastParam());
            }
        }
        else
        {
            TypeCharacterVerificationType = ResultType;
        }


        // Check the type character specified against the actual type
        //
        if (!TypeHelpers::IsBadType(TypeCharacterVerificationType))
        {
            VerifyTypeCharacterConsistency(CallLocation, TypeCharacterVerificationType, TypeCharacter);
        }
    }


    ILTree::Expression *Result = NULL;

    Result =
        AllocateExpression(
            SX_CALL,
            ResultType,
            Target,
            BoundArguments,
            CallLocation);
    Result->AsCallExpression().MeArgument = MeArgument;

    if (SomeOperandsBad)
    {
        return MakeBad(Result);
    }

    if (TargetProcedure->IsShared() ||
        TargetProcedure->IsDllDeclare() ||
        TargetProcedure->IsInstanceConstructor())
    {
        SetFlag32(Target, SXF_SYM_NONVIRT);
    }
    else if (MeArgument &&
             MeArgument->bilop == SX_SYM &&
             HasFlag32(MeArgument, (SXF_SYM_MYBASE | SXF_SYM_MYCLASS)))
    {
        SetFlag32(Target, SXF_SYM_NONVIRT);

        // Calls of the form "MyBase.Member()" are invalid if the member is
        // abstract. (Such calls are guaranteed to fail at run time.)

        if (TargetProcedure->IsMustOverrideKeywordUsed())
        {
            ReportSemanticError(
                HasFlag32(MeArgument, SXF_SYM_MYBASE)? ERRID_MyBaseAbstractCall1 : ERRID_MyClassAbstractCall1,
                Target->Loc,
                TargetProcedure);

            // This error needn't make the result bad.
        }
    }
    // Since we don't want our users to use a null reference to call
    // member functions, (i.e., dim a as class1 : a.foo() ) we won't mark this call as non-virtual (







#if 0
    else if (!TargetProcedure->IsVirtual())
    {
        SetFlag(Target, SXF_SYM_NONVIRT);
    }
#endif

    if (MeArgument &&
        TypeHelpers::IsValueTypeOrGenericParameter(MeArgument->ResultType))
    {
        if (TypeHelpers::IsValueType(TargetProcedure->GetParent()))
        {
            SetFlag32(Target, SXF_SYM_NONVIRT);
        }

        MeArgument = MakeValueTypeOrTypeParamBaseReferenceToProcedure(TargetProcedure, MeArgument, false, false);

        Result->AsCallExpression().MeArgument = MeArgument;

        if (IsBad(MeArgument))
        {
            MakeBad(Result);
        }
    }
    else
    {
        VSASSERT(
            MeArgument == NULL || TypeHelpers::IsReferenceType(MeArgument->ResultType),
            "Compiling method call on a value type with a malformed Me.");
    }

    // 

    if (!HasFlag(Flags, ExprTypeInferenceOnly) &&   // Do not optimize if we are only inferring types for anonymous types.
        m_Compiler->IsRuntimeFunctionCall(Result))
    {
        // Some runtime calls require special processing, so do this now.
        Result = OptimizeLibraryCall(Result->AsCallExpression(), Flags);

        if (IsBad(Result))
        {
            return Result;
        }

        if (IsConstant(Result))
        {
            return Result;
        }
    }
    // If we bound to the Length or LongLength member of a one-dimensional array,
    // transform this into a special tree which generates better IL (ldlen opcode).
    else if (TargetProcedure->IsPropertyGet() &&
             Result->AsCallExpression().MeArgument &&
             TypeHelpers::IsArrayType(Result->AsCallExpression().MeArgument->ResultType) &&
             Result->AsCallExpression().MeArgument->ResultType->PArrayType()->GetRank() == 1 &&
             !m_IsGeneratingXML)
    {
        if (StringPool::IsEqual(
                TargetProcedure->GetAssociatedPropertyDef()->GetName(),
                STRING_CONST(m_Compiler, Length)))
        {
            Procedure *ArrayLengthProperty =
                FindHelperMethod(
                    STRING_CONST(m_Compiler, Length),
                    GetFXSymbolProvider()->GetRootArrayType()->PClass(),
                    CallLocation,
                    true);

            if (ArrayLengthProperty &&
                ArrayLengthProperty->IsProperty() &&
                TargetProcedure == ArrayLengthProperty->PProperty()->GetProperty())
            {
                Result =
                    AllocateExpression(
                        SX_ARRAYLEN,
                        GetFXSymbolProvider()->GetIntegerType(),
                        Result->AsCallExpression().MeArgument,
                        CallLocation);
            }
        }
        else if (StringPool::IsEqual(
                    TargetProcedure->GetAssociatedPropertyDef()->GetName(),
                    STRING_CONST(m_Compiler, LongLength)))
        {
            Procedure *ArrayLongLengthProperty =
                FindHelperMethod(
                    STRING_CONST(m_Compiler, LongLength),
                    GetFXSymbolProvider()->GetRootArrayType()->PClass(),
                    CallLocation,
                    true);

            if (ArrayLongLengthProperty &&
                ArrayLongLengthProperty->IsProperty() &&
                TargetProcedure == ArrayLongLengthProperty->PProperty()->GetProperty())
            {
                Result =
                    AllocateExpression(
                        SX_ARRAYLEN,
                        GetFXSymbolProvider()->GetLongType(),
                        Result->AsCallExpression().MeArgument,
                        CallLocation);
            }
        }
    }

    if (HasFlag(Flags, ExprMustBeConstant))
    {
        ReportSemanticError(
            ERRID_RequiredConstExpr,
            CallLocation);

        MakeBad(Result);
    }

    if (HasFlag(Flags, ExprResultNotNeeded))
    {
        SetResultType(Result, TypeHelpers::GetVoidType());
    }

    return Result;
}

// Calls to some library routines can be optimized by turning them into constants.
ILTree::Expression *
Semantics::OptimizeLibraryCall
(
    ILTree::CallExpression &LibraryCall,
    ExpressionFlags Flags
)
{
    VSASSERT(!IsBad(LibraryCall) && m_Compiler->IsRuntimeFunctionCall(&LibraryCall),
             "Advertised library call isn't.");

    Type *ResultType = LibraryCall.Right->AsExpressionWithChildren().Left->ResultType;

    switch (m_Compiler->WhichRuntimeFunctionCall(&LibraryCall))
    {
        case RuntimeFunctionChr:
        {
            if (AllArgumentsConstant(LibraryCall) && !HasFlag(Flags, ExprResultNotNeeded))
            {
                // Map Chr(x) to a char constant when x is in the range [0, 128).
                Quadword ArgumentValue = LibraryCall.Right->AsExpressionWithChildren().Left->AsIntegralConstantExpression().Value;

                if (ArgumentValue >= 0 && ArgumentValue < 128)
                {
                    return
                        ProduceConstantExpression(
                            ArgumentValue,
                            LibraryCall.Loc,
                            GetFXSymbolProvider()->GetCharType()
                            IDE_ARG(LibraryCall.Right->AsExpressionWithChildren().Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                }
                // we allow all representations of values which fit in two bytes
                else if (ArgumentValue < -32768 || ArgumentValue > 65535)
                {
                    ReportSemanticError(
                        ERRID_CannotConvertValue2,
                        LibraryCall.Loc,
                        ArgumentValue,
                        GetFXSymbolProvider()->GetCharType());

                    return MakeBad(LibraryCall);
                }
            }
        }
        break;

        case RuntimeFunctionChrW:
        {
            if (AllArgumentsConstant(LibraryCall) && !HasFlag(Flags, ExprResultNotNeeded))
            {
                // Map ChrW(x) to a char constant.
                Quadword ArgumentValue = LibraryCall.Right->AsExpressionWithChildren().Left->AsIntegralConstantExpression().Value;

                // we allow all representations of values which fit in two bytes
                if (ArgumentValue < -32768 || ArgumentValue > 65535)
                {
                    ReportSemanticError(
                        ERRID_CannotConvertValue2,
                        LibraryCall.Loc,
                        ArgumentValue,
                        GetFXSymbolProvider()->GetCharType());


                    return MakeBad(LibraryCall);
                }
                else
                {
                    return
                        ProduceConstantExpression(
                            ArgumentValue & 0xFFFF,  // Dev10 #552171 And original value with 0xFFFF as ChrW does.
                            LibraryCall.Loc,
                            GetFXSymbolProvider()->GetCharType()
                            IDE_ARG(LibraryCall.Right->AsExpressionWithChildren().Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                }
            }
        }
        break;

        case RuntimeFunctionAsc:
        {
            if (AllArgumentsConstant(LibraryCall) && !HasFlag(Flags, ExprResultNotNeeded))
            {
                // Map Asc(x) to an integer constant.
                if (TypeHelpers::IsStringType(ResultType))
                {
                    if (GetStringLength(LibraryCall.Right->AsExpressionWithChildren().Left) == 0)
                    {
                        ReportSemanticError(
                            ERRID_CannotConvertValue2,
                            LibraryCall.Loc,
                            GetStringSpelling(LibraryCall.Right->AsExpressionWithChildren().Left),
                            GetFXSymbolProvider()->GetIntegerType());

                        return MakeBad(LibraryCall);
                    }
                    else if (GetStringSpelling(LibraryCall.Right->AsExpressionWithChildren().Left)[0] < 128)
                    {
                        return
                            ProduceConstantExpression(
                                GetStringSpelling(LibraryCall.Right->AsExpressionWithChildren().Left)[0],
                                LibraryCall.Loc,
                                GetFXSymbolProvider()->GetIntegerType()
                                IDE_ARG(LibraryCall.Right->AsExpressionWithChildren().Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }
                else
                {
                    VSASSERT(TypeHelpers::IsCharType(ResultType), "expected string or char type");

                    Quadword ArgumentValue = LibraryCall.Right->AsExpressionWithChildren().Left->AsIntegralConstantExpression().Value;

                    if (ArgumentValue < 128)
                    {
                        return
                            ProduceConstantExpression(
                                ArgumentValue,
                                LibraryCall.Loc,
                                GetFXSymbolProvider()->GetIntegerType()
                                IDE_ARG(LibraryCall.Right->AsExpressionWithChildren().Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }
            }
        }
        break;

        case RuntimeFunctionAscW:
        {
            if (AllArgumentsConstant(LibraryCall) && !HasFlag(Flags, ExprResultNotNeeded))
            {
                // Map AscW(x) to an integer constant.
                if (TypeHelpers::IsStringType(ResultType))
                {
                    if (GetStringLength(LibraryCall.Right->AsExpressionWithChildren().Left) == 0)
                    {
                        ReportSemanticError(
                            ERRID_CannotConvertValue2,
                            LibraryCall.Loc,
                            GetStringSpelling(LibraryCall.Right->AsExpressionWithChildren().Left),
                            GetFXSymbolProvider()->GetIntegerType());

                        return MakeBad(LibraryCall);
                    }
                    else
                    {
                        return
                            ProduceConstantExpression(
                                GetStringSpelling(LibraryCall.Right->AsExpressionWithChildren().Left)[0],
                                LibraryCall.Loc,
                                GetFXSymbolProvider()->GetIntegerType()
                                IDE_ARG(LibraryCall.Right->AsExpressionWithChildren().Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }
                else
                {
                    VSASSERT(TypeHelpers::IsCharType(ResultType), "expected string or char type");

                    Quadword ArgumentValue = LibraryCall.Right->AsExpressionWithChildren().Left->AsIntegralConstantExpression().Value;

                    return
                        ProduceConstantExpression(
                            ArgumentValue,
                            LibraryCall.Loc,
                            GetFXSymbolProvider()->GetIntegerType()
                            IDE_ARG(LibraryCall.Right->AsExpressionWithChildren().Left->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                }
            }
            else if (TypeHelpers::IsCharType(ResultType) && !HasFlag(Flags, ExprResultNotNeeded))
            {
                // Otherwise, turn this into a conversion if it's Char.  Note this special case purposely
                // performs an otherwise illegal conversion from Char to Integer.

                return
                    AllocateExpression(
                        SX_CTYPE,
                        GetFXSymbolProvider()->GetIntegerType(),
                        LibraryCall.Right->AsExpressionWithChildren().Left,
                        LibraryCall.Loc);
            }
        }
        break;

        default:

            VSFAIL("Unimplemented compile-time evaluation of constant library call.");
            return MakeBad(LibraryCall);
    }

    // If the call can't be evaluated, return it unevaluated.

    return &LibraryCall;
}

ExpressionList *
Semantics::ConstructLateBoundArgumentList
(
    ExpressionList *InterpretedArguments,
    const Location &CallLocation,
    bool LateBoundAssignment,
    bool NeedAssignmentInfo,
    ExpressionList *&CopyOutArguments,
    ILTree::Expression *&AssignmentInfoArrayParam
)
{
    // Arguments that are LValues and that end up matching a ByRef
    // parameter are effectively passed by CopyIn/CopyOut. To achieve
    // this, for each late-bound call the runtime produces an array
    // of boolean values that describes which arguments match ByRef
    // parameters. For each LValue argument, then, the compiler attaches
    // a code fragment to the late-bound call equivalent to:
    //
    //      if (ByRefArguments[ArgumentIndex])
    //          Argument = Arguments[ArgumentIndex]

    unsigned NamedArgumentCount = 0;
    unsigned ArgumentCount = 0;
    ExpressionList *NameArguments = NULL;
    ExpressionList **NameArgumentTarget = &NameArguments;
    bool SomeArgumentsBad = false;

    ExpressionList *ArgumentAssignments = NULL;
    ExpressionList **ArgumentAssignmentsTarget = &ArgumentAssignments;

    ExpressionList *ConditionalAssignments = NULL;
    ExpressionList **ConditionalAssignmentsTarget = &ConditionalAssignments;

    // AssignmentInfoArray represents an array of boolean values filled in by the
    // late-bound mechanism to describe which arguments match ByRef parameters.
    bool Need----igmentInfoArray = false;

    ExpressionList *AssignmentInfoElements = NULL;
    ExpressionList **AssignmentInfoElementTarget = &AssignmentInfoElements;

    for (ExpressionList *Arguments = InterpretedArguments;
         Arguments;
         Arguments = Arguments->AsExpressionWithChildren().Right)
    {
        ILTree::Expression *Argument = NULL;
        ILTree::ArgumentExpression *ArgumentHolder = NULL;
        Location ArgumentLocation;
        bool NamedArgument = false;

        if (Arguments->AsExpressionWithChildren().Left)
        {
            ArgumentHolder = &Arguments->AsExpressionWithChildren().Left->AsArgumentExpression();
            ArgumentLocation = ArgumentHolder->Loc;
            NamedArgument = HasFlag32(ArgumentHolder, SXF_ARG_NAMED);
            Argument = ArgumentHolder->Left;
        }
        else
        {
            // No argument is supplied. Pass System.Reflection.Missing.Value
            ArgumentLocation = Arguments->Loc;
            Argument = MakeMissingArgument(ArgumentLocation);
        }

        if (IsBad(Argument))
        {
            SomeArgumentsBad = true;
        }
        else
        {
            if (NeedAssignmentInfo)
            {
                int ElementValue;

                if ((HasFlag32(Argument, SXF_LVALUE) &&
                       // Don't try to assign back to arguments that are captured
                       // temporaries.
                       Argument->bilop != SX_ASG &&
                       Argument->bilop != SX_SEQ_OP2) ||
                    (IsPropertyReference(Argument) &&
                       AssignmentPossible(Argument->AsPropertyReferenceExpression())))
                {
                    // The argument is a valid assignment target. Save a copy
                    // of the argument so that an assignment to it can be generated
                    // posterior to the late-bound call.

                    ILTree::Expression *ArgumentAsAssignmentTarget = NULL;
                    UseTwiceShortLived(Argument, Argument, ArgumentAsAssignmentTarget);

                    ArgumentAsAssignmentTarget =
                        AllocateExpression(
                            SX_LIST,
                            TypeHelpers::GetVoidType(),
                            ArgumentAsAssignmentTarget,
                            NULL,
                            ArgumentAsAssignmentTarget->Loc);

                    ArgumentAsAssignmentTarget->LateBoundCallArgumentIndex = ArgumentCount;

                    *ArgumentAssignmentsTarget = ArgumentAsAssignmentTarget;
                    ArgumentAssignmentsTarget = &ArgumentAsAssignmentTarget->AsExpressionWithChildren().Right;

                    // Because there is at least one LValue argument, the late-bound
                    // call includes an array of booleans indicating which arguments
                    // matched ByRef parameters.

                    Need----igmentInfoArray = true;
                    ElementValue = COMPLUS_TRUE;
                }
                else
                {
                    ElementValue = COMPLUS_FALSE;
                }

                ILTree::Expression *AssignmentInfoElement =
                    ProduceConstantExpression(ElementValue, ArgumentLocation, GetFXSymbolProvider()->GetBooleanType() IDE_ARG(0));

                AssignmentInfoElement =
                    AllocateExpression(
                        SX_LIST,
                        TypeHelpers::GetVoidType(),
                        AssignmentInfoElement,
                        NULL,
                        AssignmentInfoElement->Loc);

                *AssignmentInfoElementTarget = AssignmentInfoElement;
                AssignmentInfoElementTarget = &AssignmentInfoElement->AsExpressionWithChildren().Right;
            }


            Argument =
                ConvertWithErrorChecking(
                    Argument,
                    GetFXSymbolProvider()->GetObjectType(),
                    ExprNoFlags);

            if (IsBad(Argument))
            {
                SomeArgumentsBad = true;
            }
        }

        VSASSERT(
            !IsBad(Argument) || SomeArgumentsBad,
            "ConstructLateBoundArgumentList: inconsistent badness");

        if (NamedArgument)
        {
            ILTree::Expression *NameArgument =
                ProduceStringConstantExpression(
                    ArgumentHolder->Name->Name,
                    wcslen(ArgumentHolder->Name->Name),
                    ArgumentHolder->Loc
                    IDE_ARG(0));

            NameArgument =
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    NameArgument,
                    NULL,
                    NameArgument->Loc);

            *NameArgumentTarget = NameArgument;
            NameArgumentTarget = &NameArgument->AsExpressionWithChildren().Right;

            NamedArgumentCount++;
        }

        Arguments->AsExpressionWithChildren().Left = Argument;
        ArgumentCount++;
    }

    if (SomeArgumentsBad)
    {
        return AllocateBadExpression(InterpretedArguments->Loc);
    }

    // Named arguments must be grouped at the front of the param array
    // (this is what the runtime expects), but the order of expression
    // evaluation must be preserved. Therefore, we use an array to
    // describe the order in which we fill the param array.
    // For the common case, we start filling the param array
    // somewhere in the middle and wrap around to the front when it's
    // time to evaluate the named arguments.
    //
    // Ex:  foo(a, b, c, n1:=d, n2:=e)  fills the param array in the order
    //      {2, 3, 4, 0, 1}
    //      (These are indices into the param array.)
    //
    // LateBound assignment is tricky because the last argument is unnamed
    // (and therefore must occupy the last slot in the param array).
    // Therefore, we initially ignore the last argument, then tack it on at the end
    //
    // Ex:  foo(a, b, c, n1:=d, n2:=e) = f  will fill in the order
    //      {2, 3, 4, 0, 1, 5}

    unsigned *StorageIndices = NULL;

    // The fill order is important only if there is a mix of named
    // and unnamed arguments.

    if (NamedArgumentCount > 0 && ArgumentCount != NamedArgumentCount)
    {
        StorageIndices = new(m_TreeStorage) unsigned[ArgumentCount];                    // the buffer which holds the fill order
        unsigned RingBound = LateBoundAssignment ? ArgumentCount - 1 : ArgumentCount;   // ignore the last argument if we have late bound assignment

        // the main fill order loop
        for (unsigned i = 0; i < RingBound; i++)
        {
            StorageIndices[i] = (NamedArgumentCount + i) % RingBound;
        }

        // tack the last argument onto the end
        if (LateBoundAssignment)
        {
            StorageIndices[ArgumentCount - 1] = ArgumentCount - 1;
        }
    }

    // First the argument array
    ArrayType *ParamArrayType = NULL;
    ParamArrayType = m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetObjectType());

    ILTree::Expression *ParamArray =
        InitializeArray(
            InterpretedArguments,
            ParamArrayType,
            NULL,
            InterpretedArguments ? InterpretedArguments->Loc : CallLocation,
            StorageIndices);

    Variable *ArgumentArrayTemporary = NULL;

    if (IsBad(ParamArray))
    {
        SomeArgumentsBad = true;
    }
    else if (Need----igmentInfoArray)
    {
        // There will be references to the argument array in assigning back
        // to the arguments, so the argument array must be captured to a
        // temporary.

        ILTree::ExpressionWithChildren *ArgumentArrayCapture =
            CaptureInShortLivedTemporary(ParamArray, ArgumentArrayTemporary);

        SetResultType(ArgumentArrayCapture, ParamArray->ResultType);
        ParamArray = ArgumentArrayCapture;
    }

    // Now the string array of names
    ILTree::Expression *NameArray;
    ParamArrayType = m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetStringType());

    if (NamedArgumentCount != 0)
    {
        NameArray =
            InitializeArray(
                NameArguments,
                ParamArrayType,
                NULL,
                NameArguments ? NameArguments->Loc : CallLocation);

        if (IsBad(NameArguments))
        {
            SomeArgumentsBad = true;
        }
    }
    else
    {
        NameArray = AllocateExpression(SX_NOTHING, ParamArrayType, CallLocation);
    }

    ILTree::Expression *Result =
        AllocateExpression(
            SX_LIST,
            TypeHelpers::GetVoidType(),
            ParamArray,
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                NameArray,
                NULL,
                NameArray->Loc),
            ParamArray->Loc);

    if (SomeArgumentsBad)
    {
        MakeBad(Result);
    }

    else if (Need----igmentInfoArray)
    {
        Variable *AssignmentInfoArray = NULL;

        AssignmentInfoArrayParam =
            InitializeArray(
                AssignmentInfoElements,
                m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetBooleanType()),
                NULL,
                CallLocation,
                StorageIndices,
                AssignmentInfoArray);

        if (IsBad(AssignmentInfoArrayParam))
        {
            MakeBad(Result);
        }
        else
        {
            VSASSERT(AssignmentInfoArray, "Array temporary must be allocated by this point.");

            // Generate (conditional) assignments back to the assignable arguments.

            for (ExpressionList *AssignmentListEntry = ArgumentAssignments;
                AssignmentListEntry;
                AssignmentListEntry = AssignmentListEntry->AsExpressionWithChildren().Right)
            {
                // Each copy out assignment is of the form:
                //
                // AssignmentInfoArray[ArgumentIndex] ?
                //     Argument = ResultArguments[ArgumentIndex]

                ILTree::Expression *AssignmentTarget = AssignmentListEntry->AsExpressionWithChildren().Left;

                unsigned ArgumentIndex =
                    StorageIndices ?
                        StorageIndices[AssignmentListEntry->LateBoundCallArgumentIndex] :
                        AssignmentListEntry->LateBoundCallArgumentIndex;

                Type *ArgumentType = AssignmentTarget->ResultType;

                ILTree::Expression *ResultArgument =
                    AllocateExpression(
                        SX_INDEX,
                        GetFXSymbolProvider()->GetObjectType(),
                        AllocateSymbolReference(
                            ArgumentArrayTemporary,
                            GetDataType(ArgumentArrayTemporary),
                            NULL,
                            CallLocation),
                        AllocateExpression(
                            SX_LIST,
                            TypeHelpers::GetVoidType(),
                            ProduceConstantExpression(
                                ArgumentIndex,
                                CallLocation,
                                GetFXSymbolProvider()->GetIntegerType()
                                IDE_ARG(0)),
                            NULL,
                            CallLocation),
                        CallLocation);
                ResultArgument->AsIndexExpression().DimensionCount = 1;

                if (!TypeHelpers::IsRootObjectType(ArgumentType))
                {
                    // Call ChangeType to perform a latebound conversion

                    ClassOrRecordType *ConversionsClass =
                        FindHelperClass(
                            m_Project != NULL && m_Project->GetVBRuntimeKind() == EmbeddedRuntime ?
                                STRING_CONST(m_Compiler, LateBinderConversions) :
                                STRING_CONST(m_Compiler, Conversions),
                            MicrosoftVisualBasicCompilerServicesNamespace,
                            CallLocation);

                    if (ConversionsClass == NULL)
                    {
                        return AllocateBadExpression(CallLocation);
                    }

                    Procedure *Method =
                        FindHelperMethod(STRING_CONST(m_Compiler, ChangeType), ConversionsClass, CallLocation);

                    if (Method == NULL)
                    {
                        return AllocateBadExpression(CallLocation);
                    }

                    if (!GetFXSymbolProvider()->IsTypeAvailable(FX::TypeType))
                    {
                        ReportMissingType(FX::TypeType, CallLocation);
                        return AllocateBadExpression(CallLocation);
                    }

                    ResultArgument =
                        InterpretCallExpressionWithNoCopyout(
                            CallLocation,
                            AllocateSymbolReference(
                                Method,
                                TypeHelpers::GetVoidType(),
                                NULL,
                                CallLocation),
                            chType_NONE,
                            AllocateExpression(
                                SX_LIST,
                                TypeHelpers::GetVoidType(),
                                AllocateExpression(
                                    SX_ARG,
                                    TypeHelpers::GetVoidType(),
                                    ResultArgument,
                                    CallLocation),
                                AllocateExpression(
                                    SX_LIST,
                                    TypeHelpers::GetVoidType(),
                                    AllocateExpression(
                                        SX_ARG,
                                        TypeHelpers::GetVoidType(),
                                        AllocateExpression(
                                            SX_METATYPE,
                                            GetFXSymbolProvider()->GetTypeType(),
                                            AllocateExpression(
                                                SX_NOTHING,
                                                ArgumentType,
                                                CallLocation),
                                            CallLocation),
                                        CallLocation),
                                    CallLocation),
                                CallLocation),
                            false,
                            ExprNoFlags,
                            NULL);

                    ResultArgument =
                        Convert(
                            ResultArgument,
                            ArgumentType,
                            ExprHasDirectCastSemantics,
                            ConversionNarrowing);
                }

                ILTree::Expression *Assignment =
                    GenerateAssignment(
                        CallLocation,
                        AssignmentTarget,
                        ResultArgument,
                        true);

                ILTree::Expression *MatchedByRefParameter =
                    AllocateExpression(
                        SX_INDEX,
                        GetFXSymbolProvider()->GetBooleanType(),
                        AllocateSymbolReference(
                            AssignmentInfoArray,
                            GetDataType(AssignmentInfoArray),
                            NULL,
                            CallLocation),
                        AllocateExpression(
                            SX_LIST,
                            TypeHelpers::GetVoidType(),
                            ProduceConstantExpression(
                                ArgumentIndex,
                                CallLocation,
                                GetFXSymbolProvider()->GetIntegerType()
                                IDE_ARG(0)),
                            NULL,
                            CallLocation),
                        CallLocation);
                MatchedByRefParameter->AsIndexExpression().DimensionCount = 1;

                ILTree::Expression *ConditionalAssignment =
                    AllocateExpression(
                        SX_IF,
                        TypeHelpers::GetVoidType(),
                        MatchedByRefParameter,
                        Assignment,
                        CallLocation);

                ConditionalAssignment =
                    AllocateExpression(
                        SX_LIST,
                        TypeHelpers::GetVoidType(),
                        ConditionalAssignment,
                        NULL,
                        CallLocation);

                *ConditionalAssignmentsTarget = ConditionalAssignment;
                ConditionalAssignmentsTarget = &ConditionalAssignment->AsExpressionWithChildren().Right;
            }

            CopyOutArguments = ConditionalAssignments;
        }
    }

    return Result;
}

ILTree::Expression *
Semantics::InterpretLateBoundExpression
(
    const Location &ExpressionLocation,
    LateReferenceExpression &LateReference,
    ParseTree::ArgumentList *Arguments,
    ExpressionFlags Flags
)
{
    bool SomeArgumentsBad = false;
    ExpressionList *BoundArguments = InterpretArgumentList(Arguments, SomeArgumentsBad, ExprNoFlags);

    if (SomeArgumentsBad)
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    return InterpretLateBoundExpression(ExpressionLocation, LateReference, BoundArguments, Flags);
}

ILTree::Expression *
Semantics::InterpretLateBoundExpression
(
    const Location &ExpressionLocation,
    LateReferenceExpression &LateReference,
    ExpressionList *Arguments,
    ExpressionFlags Flags
)
{
    ILTree::ExpressionWithChildren &Target = LateReference.Left->AsExpressionWithChildren();
    ExpressionList *CopyOutArguments = NULL;
    ILTree::Expression *AssignmentInfoArrayParam = NULL;

    // No copy out is needed for late calls which cannot have byref arguments (LateIndexGet/Set and LateSet).
    // We also do not care about copy out trees when generating XML (VSW#189878).
    bool NeedAssignmentInfo = (Target.bilop == SX_LATE && !HasFlag(Flags, ExprIsPropertyAssignment) && !m_IsGeneratingXML);

    Target.Right =
        ConstructLateBoundArgumentList(
            Arguments,
            ExpressionLocation,
            HasFlag(Flags, ExprIsPropertyAssignment),
            NeedAssignmentInfo,
            CopyOutArguments,
            AssignmentInfoArrayParam);

    if (IsBad(Target.Right))
    {
        MakeBad(Target);
    }

    SetLateCallInvocationProperties(Target.AsBinaryExpression(), Flags);

    if (NeedAssignmentInfo)
    {
        Target.AsLateBoundExpression().AssignmentInfoArrayParam = AssignmentInfoArrayParam;
    }

    ILTree::Expression *Result = &Target;

    if (!IsBad(Result))
    {
        Result = AppendCopyOutArguments(Result, CopyOutArguments, Flags);
    }

    return Result;
}

void
Semantics::SetLateCallInvocationProperties
(
    ILTree::BinaryExpression &Target,
    ExpressionFlags Flags
)
{
    if (HasFlag(Flags, ExprIsPropertyAssignment))
    {
        SetResultType(Target, TypeHelpers::GetVoidType());
        VSASSERT(SXE_LATE_SET == SXE_VARINDEX_SET, "VarIndex and Late flags expected to match.");
        SetFlag32(Target, SXF_ENUMtoFLAG(SXE_LATE_SET));
    }
    else if (HasFlag(Flags, ExprResultNotNeeded))
    {
        VSASSERT(Target.bilop == SX_LATE, "Expected Late isn't.");
        SetResultType(Target, TypeHelpers::GetVoidType());
        SetFlag32(Target, SXF_ENUMtoFLAG(SXE_LATE_CALL));
    }
    else
    {
        SetResultType(Target, GetFXSymbolProvider()->GetObjectType());
        VSASSERT(SXE_LATE_GET == SXE_VARINDEX_GET, "VarIndex and Late flags expected to match.");
        SetFlag32(Target, SXF_ENUMtoFLAG(SXE_LATE_GET));
        ClearFlag32(Target, SXF_LVALUE);
    }
}

ILTree::Expression *
Semantics::AppendCopyOutArguments
(
    ILTree::Expression *Result,
    ExpressionList *CopyOutArguments,
    ExpressionFlags Flags
)
{
    // If there are arguments that require copying out, add the assignments to
    // them as postoperands to the call.

    if (CopyOutArguments)
    {
        for (ExpressionList *CopyOut = CopyOutArguments; CopyOut; CopyOut = CopyOut->AsExpressionWithChildren().Right)
        {
            Result =
                AllocateExpression(
                    HasFlag(Flags, ExprResultNotNeeded) ? SX_SEQ : SX_SEQ_OP1,
                    Result->ResultType,
                    Result,
                    CopyOut->AsExpressionWithChildren().Left,
                    Result->Loc);
        }
    }

    return Result;
}

ILTree::Expression *
Semantics::InterpretObjectIndexReference
(
    const Location &ExpressionLocation,
    ILTree::Expression *ArrayRef,
    ParseTree::ArgumentList *UnboundIndices
)
{
    // Variant indexes are treated like property accesses, because that's how they
    // are treated in the trees.

    bool SomeArgumentsBad = false;
    ExpressionList *Indices = InterpretArgumentList(UnboundIndices, SomeArgumentsBad, ExprNoFlags);

    ILTree::Expression *Result =
        AllocateExpression(
            SX_VARINDEX,
            GetFXSymbolProvider()->GetObjectType(),
            ArrayRef,
            NULL,
            ExpressionLocation);

    SetResultType(Result, GetFXSymbolProvider()->GetObjectType());
    SetFlag32(Result, SXF_LVALUE);
    if (!HasFlag32(ArrayRef, SXF_LVALUE))
    {
        SetFlag32(Result, SXF_LATE_RVALUE_BASE);
    }

    if (SomeArgumentsBad)
    {
        return MakeBad(Result);
    }

    // By wrapping the variant index in a late property reference, proper
    // treatment of the variant index falls out of the rest of the compiler.

    Result =
        AllocateExpression(
            SX_LATE_REFERENCE,
            GetFXSymbolProvider()->GetObjectType(),
            Result,
            Indices,
            ExpressionLocation);

    return Result;
}

ExpressionList *
Semantics::InterpretArrayIndices
(
    ParseTree::ArgumentList *UnboundIndices,
    bool ForRedim,
    unsigned &IndexCount,
    bool &SomeOperandsBad
)
{
    IndexCount = 0;
    SomeOperandsBad = false;

    ExpressionList *BoundIndices = NULL;
    ExpressionList **IndicesTarget = &BoundIndices;

    // Count and interpret the indices.
    // The indices cannot include any elided or named operands.

    for (ParseTree::ArgumentList *Arguments = UnboundIndices;
         Arguments;
         Arguments = Arguments->Next)
    {
        ParseTree::Argument *Argument = Arguments->Element;

        if (Argument->Value == NULL)
        {
            ReportSemanticError(
                ERRID_MissingSubscript,
                Arguments->Element->TextSpan);

            SomeOperandsBad = true;
        }
        else
        {
            if (Argument->Name.Name)
            {
                ReportSemanticError(
                    ERRID_NamedSubscript,
                    Argument->TextSpan);

                SomeOperandsBad = true;
            }

            ILTree::Expression *Operand =
                InterpretExpressionWithTargetType(
                    Argument->Value,
                    ExprScalarValueFlags,
                    GetFXSymbolProvider()->GetIntegerType());

            if (IsBad(Operand))
            {
                SomeOperandsBad = true;
            }
            else if (ForRedim)
            {
                if (Operand->bilop == SX_CNS_INT)
                {
                    Operand->AsIntegralConstantExpression().Value++;
                    // 
                    if (Operand->AsIntegralConstantExpression().Value < 0)
                    {
                        ReportSemanticError(
                            ERRID_NegativeArraySize,
                            Operand->Loc);
                    }
                }
                else
                {
                    ILTree::Expression *One =
                        ProduceConstantExpression(1, Operand->Loc, GetFXSymbolProvider()->GetIntegerType() IDE_ARG(0));

                    Operand =
                        AllocateExpression(
                            SX_ADD,
                            GetFXSymbolProvider()->GetIntegerType(),
                            Operand,
                            One,
                            Operand->Loc);
                }
            }

            ExpressionList *ListElement =
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    Operand,
                    NULL,
                    Argument->TextSpan);

            *IndicesTarget = ListElement;
            IndicesTarget = &ListElement->AsExpressionWithChildren().Right;
        }

        IndexCount++;
    }

    return BoundIndices;
}

ILTree::Expression *
Semantics::InterpretArrayIndexReference
(
    const Location &ExpressionLocation,
    ILTree::Expression *ArrayRef,
    const ParseTree::ParenthesizedArgumentList &UnboundIndices
)
{
    bool SomeOperandsBad ;
    unsigned IndexCount;
    ExpressionList *BoundIndices = InterpretArrayIndices(UnboundIndices.Values, false, IndexCount, SomeOperandsBad);

    VSASSERT(TypeHelpers::IsArrayType(ArrayRef->ResultType), "Expected an array.");

    // Verify that the number of indices provided is correct.

    ArrayType *ReferencedArrayType = ArrayRef->ResultType->PArrayType();
    unsigned ExpectedIndexCount = ReferencedArrayType->GetRank();

    if (IndexCount != ExpectedIndexCount)
    {
        ReportSemanticError(
            IndexCount > ExpectedIndexCount ?
                ERRID_TooManyIndices :
                ERRID_TooFewIndices,
            UnboundIndices.TextSpan);

        SomeOperandsBad = true;
    }

    Type *ElementType = TypeHelpers::GetElementType(ReferencedArrayType);

    if (TypeHelpers::IsBadType(ElementType))
    {
        SomeOperandsBad = true;
    }

    ILTree::Expression *Result =
        AllocateExpression(
            SX_INDEX,
            ElementType,
            ArrayRef,
            BoundIndices,
            ExpressionLocation);
    Result->AsIndexExpression().DimensionCount = IndexCount;
    SetFlag32(Result, SXF_LVALUE);

    if (SomeOperandsBad)
    {
        MakeBad(Result);
    }

    return Result;
}

Declaration *
ReferencedSymbol
(
    ILTree::Expression *Input,
    bool AssertIfReturnNULL /* = true */
)
{
    while (Input->bilop == SX_INDEX)
    {
        Input = Input->AsIndexExpression().Left;
    }

    if (Input->bilop == SX_SYM)
    {
        return Input->AsSymbolReferenceExpression().Symbol;
    }

    if (Input->bilop == SX_PROPERTY_REFERENCE)
    {
        return ReferencedSymbol(Input->AsPropertyReferenceExpression().Left, AssertIfReturnNULL);
    }
    if (Input->bilop == SX_CALL)
    {
        return ReferencedSymbol(Input->AsCallExpression().Left, AssertIfReturnNULL);
    }

    if (AssertIfReturnNULL)
    {
        VSFAIL("Unknown expression as loop control variable");
    }

    VSFAIL("Unknown expression as loop control variable");
    return NULL;
}

bool
Semantics::CanMakeRValue
(
    ILTree::Expression * Input
)
{
    return
        Input &&
        (
            !TypeHelpers::IsVoidType(Input->ResultType) ||
            Input->bilop == SX_ADDRESSOF ||
            Input->bilop == SX_UNBOUND_LAMBDA ||
            Input->bilop == SX_LAMBDA
        );
}

ILTree::Expression *
Semantics::MakeRValue
(
    ILTree::Expression *Input,
    BCSYM *TargetType
)
{
    if (IsPropertyReference(Input))
    {
        Input = FetchFromProperty(Input);

        if (IsBad(Input))
        {
            return Input;
        }
    }

    if (TypeHelpers::IsVoidType(Input->ResultType))
    {
        // AddressOf, pure lambda expressions, and array literals have no type, and change to another opcode in
        // conversion to a target type. They need to slip through here.
        // Microsoft 2008.Jul.22: consider: I've changed it so MakeRValue takes a TargetType. So do these cases
        // still need to slip through? None of them are rvalues...
        // Also consider: SX_NOTHING should really have Void type as well, but it doesn't. We should consider
        // changing it to Void type, and here and now we could SetResultType(Input,TargetType) in that case.
        // At the moment that work has to be done manually in CreateCoalesceIIF.
        if 
        (
            Input->bilop != SX_ADDRESSOF && 
            Input->bilop != SX_UNBOUND_LAMBDA && 
            Input->bilop != SX_LAMBDA &&
            Input->bilop != SX_NESTEDARRAYLITERAL
        )
        {
            ReportSemanticError(
                ERRID_VoidValue,
                Input->Loc);

            return MakeBad(Input);
        }
    }

    if (!IsBad(Input) && Input->ResultType->IsArrayLiteralType() && Input->bilop==SX_ARRAYLITERAL)
    {
        // Convert the array literal to its inferred array type, reporting warnings/errors if necessary
        ILTree::Expression *convertToTargetType = ConvertArrayLiteral(&Input->AsArrayLiteralExpression(), TargetType);
        if (convertToTargetType != NULL)
        {
            Input = convertToTargetType;
        }
    }

    if (!HasFlag32(Input, SXF_LVALUE))
    {
        return Input;
    }

    ClearFlag32(Input, SXF_LVALUE);
    if (Input->bilop == SX_VARINDEX)
    {
        SetFlag32(Input, SXF_ENUMtoFLAG(SXE_VARINDEX_GET));
    }

    return Input;
}

ILTree::Expression *
Semantics::MakeAddress
(
    ILTree::Expression *Input,
    bool SuppressReadonlyLValueCapture
)
{
    if (Input->bilop == SX_ADR || Input->bilop == SX_ASG_RESADR)
    {
        return Input;
    }

    // It is valid to directly address a symbol reference previously determined to be an
    // RValue. Other RValues require capturing in a temporary.

    if (!HasFlag32(Input, SXF_LVALUE))
    {

        // If the symbol is marked read-only, we have to store it to a temporary
        // then take the address of the temporary.

        if (Input->bilop == SX_SYM)
        {
            if (SuppressReadonlyLValueCapture ||
                !(Input->AsSymbolReferenceExpression().Symbol->IsVariable() &&
                  Input->AsSymbolReferenceExpression().Symbol->PVariable()->IsReadOnly()))
            {
                SetFlag32(Input, SXF_LVALUE);
            }
            else
            {
                ILTree::Expression *Result =
                    CaptureInAddressedTemporary(Input, Input->ResultType);

                VSASSERT( Result->bilop == SX_ASG_RESADR,
                                "How can the address of temporary be anything else ?");

                // This is used to mark temporary expressions involving
                // Read only variables so that a better error can be given
                // if an attempt is made to assign to them.
                //
                SetFlag32(Result, SXF_ASG_RESADR_READONLYVALUE);

                return Result;
            }
        }
        else
        {
            return CaptureInAddressedTemporary(Input, Input->ResultType);
        }
    }

    // Check for cases of needing to get the address of a captured temporary.
    // These come in as SX_SEQ_OP2 or SX_ASG.

    if (Input->bilop == SX_SEQ_OP2)
    {
        VSASSERT(
            Input->AsExpressionWithChildren().Left->bilop == SX_ASG,
            "A sequence operator marked as an LValue should have an assignment operand.");

        SetResultType(Input->AsExpressionWithChildren().Left, Input->ResultType);
        Input = Input->AsExpressionWithChildren().Left;
    }

    if (Input->bilop == SX_ASG)
    {
        Input->bilop = SX_ASG_RESADR;
        SetResultType(Input, GetPointerType(Input->ResultType));
        ClearFlag32(Input, SXF_LVALUE);

        return Input;
    }

    return
        AllocateExpression(
            SX_ADR,
            GetPointerType(Input->ResultType),
            Input,
            Input->Loc);
}

ILTree::Expression *
Semantics::ConvertDecimalValue
(
    DECIMAL SourceValue,
    Type *TargetType,
    const Location &ExpressionLocation
    IDE_ARG(unsigned Flags)
)
{
    bool Overflow = false;

    if (TypeHelpers::IsIntegralType(TargetType) || TypeHelpers::IsCharType(TargetType))
    {
        if (SourceValue.scale == 0)
        {
            Quadword ResultValue;

            // Easy case: no scale factor.
            Overflow = (SourceValue.Hi32 != 0);

            if (!Overflow)
            {
                ResultValue = (((Quadword)SourceValue.Mid32) << 32) | SourceValue.Lo32;

                Type *SourceIntegralType = NULL;

                if (SourceValue.sign)
                {
                    // The source value is negative, so we need to negate the result value.
                    // If the result type is unsigned, or the result value is already
                    // large enough that it consumes the sign bit, then we have overflowed.

                    if (TypeHelpers::IsUnsignedType(TargetType) ||
                        (unsigned __int64)ResultValue > 0x8000000000000000ui64)
                    {
                        Overflow = true;
                    }
                    else
                    {
                        ResultValue = -ResultValue;
                        SourceIntegralType = GetFXSymbolProvider()->GetLongType();
                    }
                }
                else
                {
                    SourceIntegralType = GetFXSymbolProvider()->GetUnsignedLongType();
                }

                if (!Overflow)
                {
                    return
                        ConvertIntegralValue(
                            ResultValue,
                            SourceIntegralType,
                            TargetType,
                            ExpressionLocation
                            IDE_ARG(Flags));
                }
            }
        }
        else
        {
            double ResultValue;

            // No overflow possible
            VarR8FromDec(&SourceValue, &ResultValue);

            return
                ConvertFloatingValue(
                    ResultValue,
                    TargetType,
                    ExpressionLocation
                    IDE_ARG(Flags));
        }
    }

    if (TypeHelpers::IsFloatingType(TargetType) || TypeHelpers::IsBooleanType(TargetType))
    {
        double ResultValue;

        // No overflow possible
        VarR8FromDec(&SourceValue, &ResultValue);

        return
            ConvertFloatingValue(
                ResultValue,
                TargetType,
                ExpressionLocation
                IDE_ARG(Flags));
    }

    if (TypeHelpers::IsDecimalType(TargetType))
    {
        return ProduceDecimalConstantExpression(SourceValue, ExpressionLocation IDE_ARG(Flags));
    }

    if (Overflow)
    {
        ReportSemanticError(
            ERRID_ExpressionOverflow1,
            ExpressionLocation,
            TargetType);

        return AllocateBadExpression(ExpressionLocation);
    }

    VSFAIL("Unexpected target type for decimal conversion.");
    return NULL;
}

// Conversion from double to unsigned __int64 is annoyingly implemented by the
// VC++ compiler as (unsigned __int64)(__int64)(double)val, so we have to do it by hand.
Quadword
ConvertFloatingToUI64
(
    double SourceValue
)
{
    Quadword Result = (unsigned __int64)SourceValue;

    // code below stolen from jit...
    const double two63 = 2147483648.0 * 4294967296.0;
    if (SourceValue < two63)
    {
        Result = (__int64)SourceValue;
    }
    else
    {
        Result = (__int64)(SourceValue - two63) + 0x8000000000000000i64;
    }

    return Result;
}

bool
DetectFloatingToIntegralOverflow
(
    double SourceValue,
    bool IsUnsigned
)
{
    if (IsUnsigned)
    {
        // this code is shared by fjitdef.h
        if (SourceValue < 0xF000000000000000ui64)
        {
            if (SourceValue > -1)
            {
                return false;
            }
        }
        else
        {
            double Temporary = SourceValue - 0xF000000000000000ui64;
            if (Temporary < 0x7000000000000000i64 && (__int64)Temporary < 0x1000000000000000i64)
            {
                return false;
            }
        }
    }
    else
    {
        // this code is shared by fjitdef.h
        if (SourceValue < -0x7000000000000000i64)
        {
            double Temporary = SourceValue - -0x7000000000000000i64;
            if (Temporary > -0x7000000000000000i64 && (__int64)Temporary > -0x1000000000000001i64)
            {
                return false;
            }
        }
        else
        {
            if (SourceValue > 0x7000000000000000i64)
            {
                double Temporary = SourceValue - 0x7000000000000000i64;
                if (Temporary < 0x7000000000000000i64 && (__int64)Temporary > 0x1000000000000000i64)
                {
                    return false;
                }
            }
            else
            {
                return false;
            }
        }
    }

    return true;
}

ILTree::Expression *
Semantics::ConvertFloatingValue
(
    double SourceValue,
    Type *TargetType,
    const Location &ExpressionLocation
    IDE_ARG(unsigned Flags)
)
{
    bool Overflow = false;

    if (TypeHelpers::IsBooleanType(TargetType))
    {
        return
            ConvertIntegralValue(
                SourceValue == 0.0 ? 0 : 1,
                GetFXSymbolProvider()->GetLongType(),
                TargetType,
                ExpressionLocation
                IDE_ARG(Flags));
    }

    if (TypeHelpers::IsIntegralType(TargetType) || TypeHelpers::IsCharType(TargetType))
    {
        Overflow =
            DetectFloatingToIntegralOverflow(
                SourceValue,
                TypeHelpers::IsUnsignedType(TargetType));

        if (!Overflow)
        {
            Quadword IntegralValue;
            double Temporary;
            double Floor;
            Type *SourceIntegralType;

            // VB has different rounding behavior than C by default. Ensure we
            // are using the right type of rounding

            Temporary = SourceValue + 0.5;

            // We had a number that was equally close to 2 integers.
            // We need to return the even one.
            Floor = floor(Temporary);

            if (Floor != Temporary || fmod(Temporary, 2.0) == 0)
            {
                IntegralValue = TypeHelpers::IsUnsignedLongType(TargetType) ? ConvertFloatingToUI64(Floor) : (Quadword)Floor;
            }
            else
            {
                IntegralValue = TypeHelpers::IsUnsignedLongType(TargetType) ? ConvertFloatingToUI64(Floor - 1.0) : (Quadword)(Floor - 1.0);
            }

            if (SourceValue < 0)
            {
                SourceIntegralType = GetFXSymbolProvider()->GetLongType();
            }
            else
            {
                SourceIntegralType = GetFXSymbolProvider()->GetUnsignedLongType();
            }

            return
                ConvertIntegralValue(
                    IntegralValue,
                    SourceIntegralType,
                    TargetType,
                    ExpressionLocation
                    IDE_ARG(Flags));
        }
    }

    if (TypeHelpers::IsFloatingType(TargetType))
    {
        double ResultValue = NarrowFloatingResult(SourceValue, TargetType, Overflow);

        // We have decided to ignore overflows in compile-time evaluation
        // of floating expressions.

        return ProduceFloatingConstantExpression(ResultValue, ExpressionLocation, TargetType IDE_ARG(Flags));
    }

    if (TypeHelpers::IsDecimalType(TargetType))
    {
        DECIMAL ResultValue;

        Overflow = FAILED(VarDecFromR8(SourceValue, &ResultValue));

        if (!Overflow)
        {
            return
                ConvertDecimalValue(
                    ResultValue,
                    TargetType,
                    ExpressionLocation
                    IDE_ARG(Flags));
        }
    }

    if (Overflow)
    {
        ReportSemanticError(
            ERRID_ExpressionOverflow1,
            ExpressionLocation,
            TargetType);

        return AllocateBadExpression(ExpressionLocation);
    }

    VSFAIL("Unexpected target type for floating conversion.");
    return NULL;
}

ILTree::Expression *
Semantics::ConvertIntegralValue
(
    Quadword SourceValue,
    Type *SourceType,
    Type *TargetType,
    const Location &ExpressionLocation
    IDE_ARG(unsigned Flags)
)
{
    VSASSERT( TypeHelpers::IsIntegralType(SourceType) || TypeHelpers::IsBooleanType(SourceType) || TypeHelpers::IsCharType(SourceType),
                    "Unexpected source type passed in to conversion function!!!");

    bool Overflow = false;

    if (TypeHelpers::IsIntegralType(TargetType) || TypeHelpers::IsBooleanType(TargetType) || TypeHelpers::IsCharType(TargetType))
    {
        Quadword ResultValue = NarrowIntegralResult(SourceValue, SourceType, TargetType, Overflow);

        if (m_NoIntChecks || !Overflow)
        {
            return ProduceConstantExpression(ResultValue, ExpressionLocation, TargetType IDE_ARG(Flags));
        }
    }

    if (TypeHelpers::IsStringType(TargetType))
    {
        // ISSUE: This is correct only if the input type is Char.

        WCHAR *ResultString = new(m_TreeStorage) WCHAR[2];
        ResultString[0] = (WCHAR)SourceValue;
        ResultString[1] = 0;

        return ProduceStringConstantExpression(ResultString, 1, ExpressionLocation IDE_ARG(Flags));
    }

    if (TypeHelpers::IsFloatingType(TargetType))
    {
        return
            ConvertFloatingValue(
                TypeHelpers::IsUnsignedType(SourceType) ? (double)(unsigned __int64)SourceValue : (double)SourceValue,
                TargetType,
                ExpressionLocation
                IDE_ARG(Flags));
    }

    if (TypeHelpers::IsDecimalType(TargetType))
    {
        DECIMAL ResultValue;
        memset(&ResultValue, 0, sizeof(DECIMAL));

        if (!TypeHelpers::IsUnsignedType(SourceType) && SourceValue < 0)
        {
            // Negative numbers need converted to positive and set the negative sign bit
            ResultValue.sign = DECIMAL_NEG;
            SourceValue = -SourceValue;
        }
        else
        {
            ResultValue.sign = 0;
        }

        ResultValue.Lo32 = (long)(SourceValue & 0x00000000FFFFFFFF);
        // We include the sign bit here because we negated a negative above, so the
        // only number that still has the sign bit set is the maximum negative number
        // (which has no positive counterpart)
        ResultValue.Mid32 = (long)((SourceValue & 0xFFFFFFFF00000000) >> 32);
        ResultValue.Hi32 = 0;
        ResultValue.scale = 0;

        return
            ConvertDecimalValue(
                ResultValue,
                TargetType,
                ExpressionLocation
                IDE_ARG(Flags));
    }


    if (!m_NoIntChecks && Overflow)
    {
        ReportSemanticError(
            ERRID_ExpressionOverflow1,
            ExpressionLocation,
            TargetType);

        return MakeBad(ProduceConstantExpression(SourceValue, ExpressionLocation, TargetType IDE_ARG(Flags)));
    }

    VSFAIL("Unexpected target type for integral conversion.");
    return NULL;
}

ILTree::Expression *
Semantics::ConvertStringValue
(
    _In_z_ WCHAR *Spelling,
    Type *TargetType,
    const Location &ExpressionLocation
    IDE_ARG(unsigned Flags)
)
{
    if (TypeHelpers::IsCharType(TargetType))
    {
        return
            ProduceConstantExpression(
                Spelling[0],
                ExpressionLocation,
                TargetType
                IDE_ARG(Flags));
    }

    VSFAIL("Unexpected target type for string conversion.");
    return NULL;
}

/*=======================================================================================
ConvertUsingConversionOperator

This function builds a bound tree representing a conversion using a user-defined
conversion operator.

Conversions of the form S-->T use only one user-defined conversion at a time, i.e.,
user-defined conversions are not chained together.  It may be necessary to convert to and
from intermediate types using predefined conversions to match the signature of the
user-defined conversion exactly, so the conversion "path" is comprised of at most three
parts:

    1) [ predefined conversion  S-->Sx ]
    2) User-defined conversion Sx-->Tx
    3) [ predefined conversion Tx-->T  ]

    Where Sx is the intermediate source type
      and Tx is the intermediate target type

    Steps 1 and 3 are optional given S == Sx or Tx == T.

Given the source operand, the resolved conversion operator, and the target type, build
a bound tree representing the conversion "path".
=======================================================================================*/
ILTree::Expression *
Semantics::ConvertUsingConversionOperator
(
    ILTree::Expression *Source,
    Type *TargetType,
    Procedure *OperatorMethod,
    GenericBinding *OperatorMethodGenericContext,
    ExpressionFlags Flags
)
{
    bool SomeOperandsBad = false;

    Type *IntermediateSourceType =
        TypeInGenericContext(OperatorMethod->GetFirstParam()->GetType(), OperatorMethodGenericContext);

    Type *IntermediateTargetType =
        TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext);

    if (TypeHelpers::IsBadType(IntermediateSourceType))
    {
        ReportBadType(IntermediateSourceType, Source->Loc);
        SomeOperandsBad = true;
    }

    if (TypeHelpers::IsBadType(IntermediateTargetType))
    {
        ReportBadType(IntermediateTargetType, Source->Loc);
        SomeOperandsBad = true;
    }

    ConversionClass PreCallClassification =
        ClassifyPredefinedConversion(IntermediateSourceType, Source->ResultType);

    ConversionClass PostCallClassification =
        ClassifyPredefinedConversion(TargetType, IntermediateTargetType);

    ILTree::Expression *MethodReference =
        ReferToSymbol(
            Source->Loc,
            OperatorMethod,
            chType_NONE,
            NULL,
            OperatorMethodGenericContext,
            ExprIsExplicitCallTarget);
    SetFlag32(MethodReference, SXF_SYM_NONVIRT);  // All operators are Shared.

    if (IsBad(MethodReference))
    {
        return AllocateBadExpression(Source->Loc);
    }

    // Convert the Source to the intermediate source type, make the call, then convert
    // the call to the target type.
    ILTree::Expression *CallResult =
            AllocateExpression(
                SX_CALL,
                IntermediateTargetType,
                MethodReference,
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    Convert(
                        Source,
                        IntermediateSourceType,
                        Flags,
                        PreCallClassification),
                    Source->Loc),
                Source->Loc);

    SetFlag32(CallResult, SXF_CALL_WAS_OPERATOR);
    CallResult->AsCallExpression().OperatorOpcode = SX_CTYPE;

    ILTree::Expression *Result =
        Convert(
            CallResult,
            TargetType,
            Flags,
            PostCallClassification);

    if (SomeOperandsBad)
    {
        MakeBad(Result);
    }
    return Result;
}

/*=======================================================================================
  This is the same as above (ConvertUsingConversionOperator) except that it assumes
  all the intermediary types are nullable form.
=======================================================================================*/

ILTree::Expression *
Semantics::ConvertUsingConversionOperatorWithNullableTypes
(
    ILTree::Expression *Source,
    Type *TargetType,
    Procedure *OperatorMethod,
    GenericBinding *OperatorMethodGenericContext,
    ExpressionFlags Flags
)
{
    AssertIfFalse( TypeHelpers::IsNullableType( Source->ResultType, m_CompilerHost ) );
    AssertIfFalse( TypeHelpers::IsNullableType( TargetType, m_CompilerHost ) );
    AssertIfFalse( !TypeHelpers::IsNullableType( OperatorMethod->GetFirstParam()->GetType(), m_CompilerHost ) );
    AssertIfFalse( !TypeHelpers::IsNullableType( OperatorMethod->GetType(), m_CompilerHost ) );

    bool SomeOperandsBad = false;

    if (!GetFXSymbolProvider()->IsTypeAvailable(FX::GenericNullableType))
    {
        ReportMissingType(FX::GenericNullableType, Source->Loc);
        return AllocateBadExpression(Source->Loc);
    }

    Type *IntermediateSourceType =
        GetFXSymbolProvider()->GetNullableType(
            TypeInGenericContext(OperatorMethod->GetFirstParam()->GetType(), OperatorMethodGenericContext),
            &m_SymbolCreator
            );

    Type *IntermediateTargetType =
        GetFXSymbolProvider()->GetNullableType(
            TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext),
            &m_SymbolCreator
            );

    if (TypeHelpers::IsBadType(IntermediateSourceType))
    {
        ReportBadType(IntermediateSourceType, Source->Loc);
        SomeOperandsBad = true;
    }

    if (TypeHelpers::IsBadType(IntermediateTargetType))
    {
        ReportBadType(IntermediateTargetType, Source->Loc);
        SomeOperandsBad = true;
    }

    ConversionClass PreCallClassification =
        ClassifyPredefinedConversion(IntermediateSourceType, Source->ResultType);

    ConversionClass PostCallClassification =
        ClassifyPredefinedConversion(TargetType, IntermediateTargetType);

    ILTree::Expression *MethodReference =
        ReferToSymbol(
            Source->Loc,
            OperatorMethod,
            chType_NONE,
            NULL,
            OperatorMethodGenericContext,
            ExprIsExplicitCallTarget);
    SetFlag32(MethodReference, SXF_SYM_NONVIRT);  // All operators are Shared.

    if (IsBad(MethodReference))
    {
        return AllocateBadExpression(Source->Loc);
    }

    // Convert the Source to the intermediate source type, make the call, then convert
    // the call to the target type.
    ILTree::Expression *CallResult =
            AllocateExpression(
                SX_CALL,
                IntermediateTargetType,
                MethodReference,
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    Convert(
                        Source,
                        IntermediateSourceType,
                        Flags,
                        PreCallClassification),
                    Source->Loc),
                Source->Loc);

    SetFlag32(CallResult, SXF_CALL_WAS_OPERATOR);
    CallResult->AsCallExpression().OperatorOpcode = SX_CTYPE;

    ILTree::Expression *Result =
        Convert(
            CallResult,
            TargetType,
            Flags,
            PostCallClassification);

    if (SomeOperandsBad)
    {
        MakeBad(Result);
    }
    return Result;
}

BILOP GetCTypeBilop(ExpressionFlags Flags)
{
    if (HasFlag(Flags, ExprHasDirectCastSemantics))
    {
        return SX_DIRECTCAST;
    }
    else if (HasFlag(Flags, ExprHasTryCastSemantics))
    {
        return SX_TRYCAST;
    }
    else
    {
        return SX_CTYPE;
    }
}

ILTree::Expression *
Semantics::Convert
(
    ILTree::Expression *Input,
    Type *TargetType,
    ExpressionFlags Flags,
    ConversionClass ConversionClassification
)
{
    VSASSERT(
        Input->bilop != SX_LATE_REFERENCE &&
            Input->bilop != SX_PROPERTY_REFERENCE &&
            !HasFlag32(Input, SXF_LVALUE),
        "Attempted conversion on non-Rvalue.");

    Type *SourceType = Input->ResultType;

    // XML generation needs to see all explicit conversions, even those
    // that can be processed at compile time and those with no effect.

    if (m_IsGeneratingXML &&
        HasFlag(Flags, ExprIsExplicitCast) &&
        !HasFlag(Flags, ExprMustBeConstant))
    {
        ILTree::Expression *Result =
           AllocateExpression(
                HasFlag(Flags, ExprHasDirectCastSemantics) ?
                    SX_DIRECTCAST :
                    HasFlag(Flags, ExprHasTryCastSemantics) ?
                        SX_TRYCAST :
                        SX_CTYPE,
                TargetType,
                Input,
                Input->Loc);

        SetFlag32(Result, SXF_COERCE_EXPLICIT);
        return Result;
    }

    if (SourceType->IsArrayLiteralType())
    {
        // Note: do NOT disable this assertion. If you have ArrayLiteralTypes which aren't SX_ARRAYLITERALS
        // then the Convert* routines will fail in mysterious ways, as will InterpretExpresion, and ClassifyConversion.
        VSASSERT(Input->bilop==SX_ARRAYLITERAL, "unexpected: a non-array-literal expression which claims to have a literal type");
    }
    if (SourceType->IsArrayLiteralType() && Input->bilop==SX_ARRAYLITERAL)
    {
        // When someone calls ConvertWithErrorChecking then it already catches all SourceArrayLiteral->Target conversions.
        // But if it discovered a conversion route via user-defined operators, then the ConvertThroughUserDefinedOperator
        // code ends up calling Convert rather than ConvertWithErrorChecking, and so we might end up here.
        ILTree::Expression * pResult = ConvertArrayLiteral(&Input->AsArrayLiteralExpression(), TargetType);
        VSASSERT(pResult!=NULL, "Unexpected: why would someone ask us to convert from an array literal, unless they already knew it would succeed?");
        return pResult;
    }

    if (TypeHelpers::EquivalentTypes(TargetType, SourceType) )
    {
        // Dev10 #540062 
        if (HasFlag(Flags, ExprIsExplicitCast) && HasFlag(Flags, ExprHasDirectCastSemantics))
        {
            if (TypeHelpers::IsFloatingType(TargetType))
            {
                ReportSemanticError(ERRID_IdentityDirectCastForFloat, Input->Loc);  
            }
            else if (TypeHelpers::IsValueType(TargetType))
            {
                ReportSemanticError(WRNID_ObsoleteIdentityDirectCastForValueType, Input->Loc);  
            }
        }
        else
        {
            // Why would we try to do implicit identity DirectCast for a value-type?
            // It is sufficient to Assert in this case because we will return from the 'if' below and will not produce an invalid IL.
            // Conversion will be treated as a no-op. Reporting error/warning as above in this case isn't the right thing to do
            // because it is likely that there is no explicit DirectCast operation in user's code.
            AssertIfTrue(HasFlag(Flags, ExprHasDirectCastSemantics) && TypeHelpers::IsValueType(TargetType));
        }
        
        // It is necessary to generate code for explicit
        // floating-to-floating conversions that don't change representations.
        if (!(HasFlag(Flags, ExprIsExplicitCast) && TypeHelpers::IsFloatingType(TargetType)))
        {
            return Input;
        }
    }

    // A conversion to or from an enum type that does not involve a
    // change in representation does not require a coercion, even though
    // the conversion may technically be narrowing.

    if (((TypeHelpers::IsEnumType(SourceType) || TypeHelpers::IsEnumType(TargetType)) &&
         SourceType->GetVtype() == TargetType->GetVtype()) &&
        !m_IsGeneratingXML)
    {
        if (Input->bilop == SX_CNS_INT)
        {
            SetResultType(Input, TargetType);
            return Input;
        }
        else
        {
            if (!HasFlag(Flags, ExprSuppressWideCoerce))
            {
                return AllocateExpression(SX_WIDE_COERCE, TargetType, Input, Input->Loc);
            }
            else
            {
                return AllocateExpression(GetCTypeBilop(Flags), TargetType, Input, Input->Loc);
            }
        }
    }

    if (IsConstant(Input))
    {
        if (HasFlag(Flags, ExprMustBeConstant) &&
            TypeHelpers::IsGenericParameter(TargetType) &&
            IsNothingLiteral(Input))
        {
            // Change the TargetType for type param constants to be Object so that
            // if required to be cast to the type parameter, the appropriate boxing,
            // initobj, etc can be done correctly.
            //
            // Note that constant expressions of type parameter type can only occur
            // for default values of optional parameters of typee parameter types.
            //
            Type *TargetTypeForGenericParamConstant = GetFXSymbolProvider()->GetObjectType();

            ConstantValue Zero;
            Zero.TypeCode = TargetTypeForGenericParamConstant->GetVtype();

            return ProduceConstantExpression(Zero, Input->Loc, TargetTypeForGenericParamConstant IDE_ARG(0));
        }

        if (AllowsCompileTimeConversions(TargetType) &&
            (HasFlag(Flags, ExprMustBeConstant) ||
                !m_IsGeneratingXML ||
                (TypeHelpers::IsCharType(SourceType) && TypeHelpers::IsStringType(TargetType))))
        {
            // When generating XML for Strings, we need to force the conversion from Char to String, and this must be done
            // so that designers get XML that has the string in it, not an integer casted to a string.

            if (IsNothingLiteral(Input))
            {
                // A Nothing literal turns into the default value of the target type.

                ConstantValue Zero;
                Zero.TypeCode = TargetType->GetVtype();

                return ProduceConstantExpression(Zero, Input->Loc, TargetType IDE_ARG(0));
            }

            // Microsoft: It is ok for floating points to come here with DirectCast because we will
            // attempt to type convert between single/double and it may overflow.

            // VSASSERT(!HasFlag(Flags, ExprHasDirectCastSemantics) && !HasFlag(Flags, ExprHasTryCastSemantics),
            //         "DirectCast and TryCast cannot cast between constants, so how did we get here?");

            if (AllowsCompileTimeConversions(SourceType))
            {
                // Attempt to convert the value of the constant to the result type.
                // If this succeeds, produce a constant expression
                // with the converted value.

                if (TypeHelpers::IsIntegralType(SourceType) || TypeHelpers::IsBooleanType(SourceType) || TypeHelpers::IsCharType(SourceType))
                {
                    if (TypeHelpers::IsIntegralType(TargetType) ||
                        TypeHelpers::IsBooleanType(TargetType) ||
                        TypeHelpers::IsCharType(TargetType) ||
                        TypeHelpers::IsFloatingType(TargetType) ||
                        TypeHelpers::IsDecimalType(TargetType) ||
                        (TypeHelpers::IsCharType(SourceType) && TypeHelpers::IsStringType(TargetType)))
                    {
                        Quadword Value = Input->AsIntegralConstantExpression().Value;

                        // Converting True to an arithmetic value produces -1.
                        if (TypeHelpers::IsBooleanType(SourceType) && Value != 0)
                        {
                            if (TypeHelpers::IsUnsignedType(TargetType))
                            {
                                bool Overflow = false;
                                Value = NarrowIntegralResult(BASIC_TRUE, SourceType, TargetType, Overflow);
                            }
                            else
                            {
                                Value = BASIC_TRUE;
                            }
                        }

                        return
                            ConvertIntegralValue(
                                Value,
                                SourceType,
                                TargetType,
                                Input->Loc
                                IDE_ARG(Input->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }

                else if (TypeHelpers::IsFloatingType(SourceType))
                {
                    if (TypeHelpers::IsIntegralType(TargetType) ||
                        TypeHelpers::IsBooleanType(TargetType) ||
                        TypeHelpers::IsCharType(TargetType) ||
                        TypeHelpers::IsFloatingType(TargetType) ||
                        TypeHelpers::IsDecimalType(TargetType))
                    {
                        return
                            ConvertFloatingValue(
                                Input->AsFloatConstantExpression().Value,
                                TargetType,
                                Input->Loc
                                IDE_ARG(Input->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }

                else if (TypeHelpers::IsDecimalType(SourceType))
                {
                    if (TypeHelpers::IsIntegralType(TargetType) ||
                        TypeHelpers::IsBooleanType(TargetType) ||
                        TypeHelpers::IsCharType(TargetType) ||
                        TypeHelpers::IsFloatingType(TargetType) ||
                        TypeHelpers::IsDecimalType(TargetType))
                    {
                        return
                            ConvertDecimalValue(
                                Input->AsDecimalConstantExpression().Value,
                                TargetType,
                                Input->Loc
                                IDE_ARG(Input->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }

                else if (TypeHelpers::IsStringType(SourceType))
                {
                    if (TypeHelpers::IsCharType(TargetType))
                    {
                        return ConvertStringValue(
                                GetStringSpelling(Input),
                                TargetType,
                                Input->Loc
                                IDE_ARG(Input->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
                    }
                }

                else
                {
                    VSASSERT(
                        ConversionClassification == ConversionNarrowing,
                        "Unhandled type in compile-time constant conversion.");
                }
            }
        }
    }

    // 
    if (!m_IsGeneratingXML && TypeHelpers::IsGenericParameter(TargetType))
    {
        // Converting Nothing --> T
        if (IsNothingLiteral(Input))
        {
            Variable *ResultTemporary = AllocateResultTemporary(TargetType);

            ILTree::Expression *Result =
                MakeRValue(
                    AllocateSymbolReference(
                        ResultTemporary,
                        TargetType,
                        NULL,
                        Input->Loc));

            ILTree::Expression *ReferenceToInit =
                AllocateSymbolReference(
                    ResultTemporary,
                    TargetType,
                    NULL,
                    Input->Loc);

            ILTree::Expression *Init =
                AllocateExpression(
                    SX_INIT_STRUCTURE,
                    TypeHelpers::GetVoidType(),
                    Input->Loc);

            Init->AsInitStructureExpression().StructureReference = MakeAddress(ReferenceToInit, true);
            Init->AsInitStructureExpression().StructureType = TargetType;

            return
                AllocateExpression(
                    SX_SEQ_OP2,
                    TargetType,
                    Init,
                    Result,
                    Input->Loc);
        }

        // Converting Object --> T calls a generic method helper which
        // determines the conversion to use at runtime.
        if (TypeHelpers::IsRootObjectType(SourceType) &&
            !HasFlag(Flags, ExprHasDirectCastSemantics) &&
            !HasFlag(Flags, ExprHasTryCastSemantics))
        {
            ClassOrRecordType *ConversionsClass =
                FindHelperClass(
                    STRING_CONST(m_Compiler, Conversions),
                    MicrosoftVisualBasicCompilerServicesNamespace,
                    Input->Loc);

            if (ConversionsClass == NULL)
            {
                return AllocateBadExpression(Input->Loc);
            }

            Procedure *Method =
                FindHelperMethod(STRING_CONST(m_Compiler, ToGenericParameter), ConversionsClass, Input->Loc);

            if (Method == NULL)
            {
                return AllocateBadExpression(Input->Loc);
            }

            bool ResultIsBad = false;
            const int ArgumentCount = 1;
            Type **BoundArguments = new(m_TreeStorage) Type *[ArgumentCount];
            Location *TypeArgumentLocations = new(m_TreeStorage) Location[ArgumentCount];

            BoundArguments[0] = TargetType;
            TypeArgumentLocations[0] = Input->Loc;

            GenericBinding *Binding =
                ValidateGenericArguments(
                    Input->Loc,
                    Method,
                    BoundArguments,
                    TypeArgumentLocations,
                    ArgumentCount,
                    NULL,
                    ResultIsBad);

            ILTree::SymbolReferenceExpression *MethodTree =
                AllocateSymbolReference(
                    Method,
                    Method->GetType(),
                    NULL,
                    Input->Loc);
            MethodTree->GenericBindingContext = Binding;

            ExpressionList *CopyOutArguments = NULL;

            ILTree::Expression *Result =
                InterpretCallExpression(
                    Input->Loc,
                    MethodTree,
                    chType_NONE,
                    AllocateExpression(
                        SX_LIST,
                        TypeHelpers::GetVoidType(),
                        AllocateExpression(
                            SX_ARG,
                            TypeHelpers::GetVoidType(),
                            Input,
                            Input->Loc),
                        Input->Loc),
                    CopyOutArguments,
                    ResultIsBad,
                    Flags,
                    OvrldNoFlags,
                    NULL);

            return Result;
        }
    }

    if (TypeHelpers::IsDelegateType(TargetType) &&
        TargetType != GetFXSymbolProvider()->GetMultiCastDelegateType() &&
        SourceType->IsAnonymousDelegate())
    {
        return ConvertAnonymousDelegateToOtherDelegate(Input, TargetType);
    }

    // Check for class-type conversions that don't require any run-time checking.
    // These are conversions to a base class and conversions to an
    // implemented interface.
    //
    // Conversion from 1-dimensional array of Char to String is a special case
    // of a widening conversion that requires a coercion node.

    if (ConversionClassification == ConversionWidening &&
        (TypeHelpers::IsReferenceType(SourceType) && !TypeHelpers::IsGenericParameter(SourceType)) &&
        (TypeHelpers::IsReferenceType(TargetType) && !TypeHelpers::IsGenericParameter(TargetType)) &&
        !(TypeHelpers::IsCharArrayRankOne(SourceType) && TypeHelpers::IsStringType(TargetType)))
    {
        // This is widening object reference conversion.  Put a place-holder in so
        // codegen knows when not to generate a GetObjectValue call.
        // Not necessary for Nothing because Nothing always takes on the type of the target.

        if (Input->bilop == SX_NOTHING)
        {
            SetResultType(Input, TargetType);
            return Input;
        }

        if (SourceType->IsArrayType())
        {
            Type *SourceElementType = SourceType->ChaseToType();

                // 
            if ((TargetType->IsArrayType() &&
                 (TypeHelpers::IsGenericParameter(SourceElementType) || TypeHelpers::IsGenericParameter(TargetType->ChaseToType()))) ||
                // 
                (TypeHelpers::IsGenericParameter(SourceElementType) &&
                 TargetType->IsGenericTypeBinding() &&
                 TargetType->PGenericBinding()->GetGeneric()->IsInterface() &&
                 TargetType->PGenericBinding()->GetArgumentCount() == 1 &&
                 !TypeHelpers::EquivalentTypes(TargetType->PGenericBinding()->GetArgument(0), SourceElementType)))
            {
                return AllocateExpression(SX_DIRECTCAST, TargetType, Input, Input->Loc);
            }
        }
        if (!HasFlag(Flags, ExprSuppressWideCoerce))
        {
            return AllocateExpression(SX_WIDE_COERCE, TargetType, Input, Input->Loc);
        }
        else
        {
            return AllocateExpression(GetCTypeBilop(Flags), TargetType, Input, Input->Loc);
        }
    }

    // At this point it is known that the conversion involves a coercion.
    ILTree::Expression *CoerceOperand = Input;

    // Note that the fix for 




    if ((!TypeHelpers::IsRootObjectType(TargetType) && TypeHelpers::IsGenericParameter(SourceType)) ||
        (TypeHelpers::IsInterfaceType(TargetType) && TypeHelpers::IsValueType(SourceType)) ||
        (TypeHelpers::IsGenericParameter(TargetType) && TypeHelpers::IsValueType(SourceType)))
    {
        // For any conversion from a generic parameter to any other type,
        // box the operand and then allow the explicit cast from Object to
        // the target type. This is a clr requirement.

        // For any conversion from a value type to an implemented interface,
        // box the operand and then allow the explicit conversion from Object to
        // the interface below.

        // For any conversion from a value type to a generic parameter,
        // box the operand and then allow the explicit cast from Object to
        // the target generic parameter type.

       CoerceOperand =
            AllocateExpression(
                SX_DIRECTCAST,
                GetFXSymbolProvider()->GetObjectType(),
                CoerceOperand,
                CoerceOperand->Loc);
    }


    // The fix for 
    ILTree::Expression *Result =
        AllocateExpression(
            HasFlag(Flags, ExprHasDirectCastSemantics) ?
                SX_DIRECTCAST :
                HasFlag(Flags, ExprHasTryCastSemantics) ?
                    SX_TRYCAST :
                    (TypeHelpers::IsRecordType(TargetType) && TypeHelpers::IsGenericParameter(SourceType)) ?  // 
                        SX_DIRECTCAST :
                        SX_CTYPE,
            TargetType,
            CoerceOperand,
            CoerceOperand->Loc);

    if (!HasFlag(Flags, ExprHasDirectCastSemantics) && !HasFlag(Flags, ExprHasTryCastSemantics))
    {
        if (TypeHelpers::IsRecordType(TargetType) && TypeHelpers::IsReferenceType(SourceType))
        {
            // 














            AllocateDefaultValueTemporary(TargetType, &Input->Loc);
        }
    }

    return Result;
}

ILTree::Expression *
Semantics::ConvertWithErrorChecking
(
    ILTree::Expression *Input,
    Type *TargetType,
    ExpressionFlags Flags,
    bool SuppressMethodNameInErrorMessages,
    bool * pRequiresUnwrappingNullable,
    bool IgnoreOperatorMethod
)
{
    Parameter *CopyBackConversionParam = NULL;
    bool RequiresNarrowingConversion = false;
    bool NarrowingFromNumericLiteral = false;
    DelegateRelaxationLevel DelegateRelaxationLevel = DelegateRelaxationLevelNone;
    bool tmpRequiresUnwrappingNullable = pRequiresUnwrappingNullable ? *pRequiresUnwrappingNullable : false;

    ILTree::Expression *pExpression = 
        ConvertWithErrorChecking
        (
            Input,
            TargetType,
            Flags,
            CopyBackConversionParam,
            RequiresNarrowingConversion,
            NarrowingFromNumericLiteral,
            SuppressMethodNameInErrorMessages,
            DelegateRelaxationLevel,
            tmpRequiresUnwrappingNullable,
            NULL, //pAsyncSubArgumentAmbiguity
            IgnoreOperatorMethod
        );
    
    if (pRequiresUnwrappingNullable)
    {
        *pRequiresUnwrappingNullable = tmpRequiresUnwrappingNullable;
    }
    
    return pExpression;
        
}

ILTree::ExpressionWithChildren *
Semantics::ConvertArrayLiteralElements
(
    ILTree::Expression * pLiteral,
    BCSYM * pTargetType,
    bool &RequiresNarrowingConversion,
    bool &NarrowingFromNumericLiteral
)
{
    int requiresNarrowingConversionCount = 0;
    int narrowingFromNumericLiteralCount = 0;

    ILTree::ExpressionWithChildren * result = ConvertArrayLiteralElementsHelper
                (
                    pLiteral,
                    pTargetType,
                    requiresNarrowingConversionCount,
                    narrowingFromNumericLiteralCount
                );

    if (requiresNarrowingConversionCount > 0)
    {
        RequiresNarrowingConversion = true;

        if (requiresNarrowingConversionCount == narrowingFromNumericLiteralCount)
        {
            NarrowingFromNumericLiteral = true;
        }
    }

    return result;
}

ILTree::ExpressionWithChildren *
Semantics::ConvertArrayLiteralElementsHelper
(
    ILTree::Expression * pLiteral,
    BCSYM * pTargetType,
    int &RequiresNarrowingConversion,
    int &NarrowingFromNumericLiteral
)
{
    ThrowIfNull(pLiteral);
    ThrowIfNull(pTargetType);
    ThrowIfFalse(pLiteral->bilop == SX_NESTEDARRAYLITERAL || pLiteral->bilop == SX_ARRAYLITERAL);
    
    // The conversions we do here might end up mutating the array literal (in the case that
    // any lambda parameter inference was done in it). If m_ReportErrors is set, then
    // presumably we want to mutate the array literal. If it's unset, then presumably
    // we don't, and we instead want to work off a copy. Note: only need to copy for
    // for the outermost array literal... nested array literals can assume the thing's already copied.
    //
    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    // !!  DANGER   DANGER   DANGER   DANGER   DANGER   DANGER   DANGER   DANGER   DANGER   DANGER   !!
    // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
    //
    // This presumption normally holds. But some places in the code use a TemporaryErrorTable
    // and then choose either to merge it or not to merge it. There's no mechanism in place for
    // what those places should do about a "temporary SX_ tree" and whether to merge it or not.
    //
    if (pLiteral->bilop == SX_ARRAYLITERAL && !m_ReportErrors)
    {
        pLiteral = &m_TreeAllocator.xCopyBilTreeForScratch(pLiteral)->AsArrayLiteralExpression();
    }

    ILTree::ExpressionWithChildren * pExprList = 
        pLiteral->bilop == SX_NESTEDARRAYLITERAL ? 
            pLiteral->AsNestedArrayLiteralExpression().ElementList : 
            pLiteral->AsArrayLiteralExpression().ElementList;

    ExpressionListHelper convertedArguments(this);

    while (pExprList)
    {
        ThrowIfNull(pExprList->Left);
        
        ILTree::Expression * pConvertedExpression = NULL;

        if (pExprList->Left->bilop == SX_NESTEDARRAYLITERAL)
        {
            pConvertedExpression = 
                AllocateNestedArrayLiteralExpression
                ( 
                    ConvertArrayLiteralElementsHelper(pExprList->Left, pTargetType, 
                                    RequiresNarrowingConversion,
                                    NarrowingFromNumericLiteral),
                    pExprList->Left->Loc
                );    
        }       
        else if (IsBad(pExprList->Left))
        {
            pConvertedExpression = pExprList->Left;
        }
        else
        {
            Parameter *CopyBackConversionParam = NULL;
            bool _RequiresNarrowingConversion = false;
            bool _NarrowingFromNumericLiteral = false;
            DelegateRelaxationLevel _delegateRelaxationLevel = DelegateRelaxationLevelNone;
            bool requiresUnwrappingNullable = false;
         
            pConvertedExpression = ConvertWithErrorChecking(pExprList->Left, pTargetType, ExprNoFlags,
                                    CopyBackConversionParam,
                                    _RequiresNarrowingConversion,
                                    _NarrowingFromNumericLiteral,
                                    false,
                                    _delegateRelaxationLevel,
                                    requiresUnwrappingNullable
                                );

            if (_RequiresNarrowingConversion)
            {
                RequiresNarrowingConversion++;

                if (_NarrowingFromNumericLiteral)
                {
                    NarrowingFromNumericLiteral++;
                }
            }
        }
        
        convertedArguments.Add(pConvertedExpression, pExprList->Loc);
        pExprList = pExprList->Right ? & pExprList->Right->AsExpressionWithChildren() : NULL;
    }

    return convertedArguments.Start();
}


ILTree::Expression *
Semantics::ConvertArrayLiteral
(
    ILTree::ArrayLiteralExpression * pLiteral,
    BCSYM * pTargetType
)
{
    bool requiresNarrowingConversion = false;
    bool narrowingFromNumericLiteral = false;

    return ConvertArrayLiteral
                (
                    pLiteral,
                    pTargetType,
                    requiresNarrowingConversion,
                    narrowingFromNumericLiteral
                );
}


ILTree::Expression *
Semantics::ConvertArrayLiteral
(
    ILTree::ArrayLiteralExpression * pLiteral,
    BCSYM * pTargetType,
    bool &RequiresNarrowingConversion,
    bool &NarrowingFromNumericLiteral
)
{
    // This function converts an array literal to either an array, or an
    // IEnumerable(Of T)/ICollection(Of T)/IList(Of T)/IReadOnlyList(Of T)/IReadOnlyCollection, or an IEnumerable/ICollection/IList,
    // or a System.Array, or a System.Object.
    // We return NULL if the conversion isn't allowed.
    // If converting directly to an array, then we just emit the appropriate ILTree.
    // If converting to an interface or class, we also emit the SX_WIDE_COERCE so that the CLR will castclass it.
    //
    // If pTargetType is NULL, then we will just convert the array literal according to its suggested dominant type
    // (reporting warnings/errors as appropriate, e.g. "warning: object assumed" or "error: no dominant type").
    // In this case, we are GUARANTEED to return a non-NULL expression.
    // (but if pTargetType is non-NULL, then it may be impossible to match to the target context, so we return
    // NULL to indicate failure.)
    // 
    //
    // Note: we use a BCSYM_ArrayLiteral to indicate that the array is a literal and is subject to reclassification.
    // Every BCSYM_ArrayLiteral will have an SX_ARRAYLITERAL expression.
    // But it's possible to have an SX_ARRAYLITERAL that's NOT a BCSYM_ArrayLiteral. That will happen with
    // e.g. CType({1},Integer()), where the fact that it's no longer reclassificable is indicated by
    // its type now being BCSYM_ArrayType rather than BCSYM_ArrayLiteralType.
    //
    // Argument:
    //
    // pLiteral:ILTree_ArrayLiteralExpression
    //          with ResultType:BCSYM_ArrayLiteralType = a type symbol for the literal
    //                   with type = the dominant type of the list
    //                   with elements = a flattened list of every element in the original array literal, including nested ones
    //          with ElementList = the original expressions
    //
    // We start by judging whether the array expressions need to be converted or not, and we generate
    // an appropriate intermediate array. Then we provide an SX_WIDE_COERCE if necessary.
    //
    // Note that if the overall shape of the reclassification (from array literal to whatever) is acceptable,
    // then we go ahead and do it. It might be that individual elements in the array will report errors
    // for the conversions they're asked to do. So be it. We still reclassify.


    ThrowIfNull(pLiteral);
    if (!pLiteral->ResultType->IsArrayLiteralType())
    {
        VSFAIL("Expected array literal to have an array literal type");
        ReportSemanticError(ERRID_InternalCompilerError, pLiteral->Loc);
        return MakeBad(pLiteral);
    }
    BCSYM_ArrayLiteralType *pArrayLiteralType = pLiteral->ResultType->PArrayLiteralType();


    // Dev10 introduces the /langVersion switch in which case we need to flag situations where a previous
    // version of the compiler can't process a language construct.  In this case, we need to catch the case
    // of collection initializers.  This was not legal in < vb10: dim a = {1,2}  But this was: dim a()={1,2}
    // So here we are checking to see if the left hand side is an array or not because if it isn't, then we
    // are looking at collection initializer usage that wasn't legal before vb10
    if ( !m_InitializerTargetIsArray )
    {
        // dev10 567239 - don't send the range of the entire literal in.  Just send the leading brace
        Location initializerLocation = pArrayLiteralType->GetLiteralLocation();
        Location startLoc;
        startLoc.SetStart(&initializerLocation);
        startLoc.SetEnd(initializerLocation.m_lBegLine, initializerLocation.m_lBegColumn);
        AssertLanguageFeature(FEATUREID_ArrayLiterals, startLoc);
    }

    // Here's how we'll generate an appropriate array:

    BCSYM *pIntermediateElementType;  // what will we convert each element to?
    int IntermediateRank;             // intermediate rank that we convert it?
    bool NeedsCoerce;                 // Will we have to coerce the intermediate array to the target type?

    // Here are the cases (TargetType=NULL/Object/System.Array/System.Array interfaces)
    // where the inferred dominant type is picked as the element type:
    // Dev10#491774: all these cases have to share the same code path, to pick up the same warnings/errors
    // on dominant type.
    if (pTargetType==NULL || pTargetType->IsObject() || pTargetType==GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::ArrayType) ||
        (pTargetType->IsInterface() && IsOrInheritsFromOrImplements(GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::ArrayType), pTargetType)))
    {
        pIntermediateElementType = pArrayLiteralType->GetRoot();
        IntermediateRank = pArrayLiteralType->GetRank();
        NeedsCoerce = (pTargetType!=NULL);

        bool strict = m_UsingOptionTypeStrict;
        bool custom = !strict && WarnOptionStrict();

        if (IsRestrictedType(pIntermediateElementType, m_CompilerHost))
        {
            // "'|1' cannot be made nullable, and cannot be used as the data type of an array element, field, anonymous type member, type argument, 'ByRef' parameter, or return statement."
            ReportSemanticError(ERRID_RestrictedType1, pLiteral->Loc, pIntermediateElementType);
            // Dev10#473846: normal array types are checked for restricted-types in InterpretMethodBody, within its
            // calls to CheckRestrictedArraysInLocals() and then CheckChildrenForBlockLevelShadowing().
            // I don't see the sense in postponing the check for that long. Also, the check at that point
            // requires to know the location of the symbol that declared the array type, and anyway array
            // literals don't have that location. So reporting the error here is all round better.
            // Note that what we're reporting here is solely for inferred type, e.g. "Dim x = {new ArgIterator}".
            // If instead the user wrote "Dim x as Object() = {new ArgIterator}" then the problem would be
            // reported as a conversion error from ArgIterator to Object later on. And "Dim x as ArgIterator() = {}"
            // is reported elsewhere as a bad type declaration.
            pLiteral->ResultType = m_SymbolCreator.GetVoidType();
            return MakeBad(pLiteral);
        }

        else if (pIntermediateElementType->IsVoidType())
        {
            // "A type could not be inferred from the elements of the array. Specifying the type of the array might correct this error."
            ReportSemanticError(ERRID_ArrayInitNoType, pLiteral->Loc);
            // e.g. converting {AddressOf f} to a System.Array, or just "dim x = {AddressOf f}"
            pLiteral->ResultType = m_SymbolCreator.GetVoidType();
            return MakeBad(pLiteral);
        }

        else if (pLiteral->NumDominantCandidates==0 && strict)
        {
            // "Cannot infer an element type, and Option Strict On does not allow 'Object' to be assumed. Specifying the type of the array might correct this error."
            ReportSemanticError(ERRID_ArrayInitNoTypeObjectDisallowed, pLiteral->Loc);
            pLiteral->ResultType = m_SymbolCreator.GetVoidType();
            return MakeBad(pLiteral);
        }
        else if (pLiteral->NumDominantCandidates==0 && custom)
        {
            // "Cannot infer an element type; 'Object' assumed."
            StringBuffer buf;
            BackupValue<bool> backupReportErrors(&m_ReportErrors);
            m_ReportErrors |= m_ReportMultilineLambdaReturnTypeInferenceErrors.HasValue() && m_ReportMultilineLambdaReturnTypeInferenceErrors.GetValue() && m_Errors != NULL;
            ReportSemanticError(WRNID_ObjectAssumed1, pLiteral->Loc, ResLoadString(WRNID_ArrayInitNoTypeObjectAssumed, &buf));
            // e.g. "dim x = {function(x as integer)x, function(y as string)y}"
            // This is just a warning; we stil continue.
        }
        else if (pLiteral->NumDominantCandidates>1 && strict)
        {
            // "Cannot infer an element type because more than one type is possible. Specifying the type of the array might correct this error."
            ReportSemanticError(ERRID_ArrayInitTooManyTypesObjectDisallowed, pLiteral->Loc);
            pLiteral->ResultType = m_SymbolCreator.GetVoidType();
            return MakeBad(pLiteral);
        }
        else if (pLiteral->NumDominantCandidates>1 && custom)
        {
            // "Cannot infer an element type because more than one type is possible; 'Object' assumed."
            StringBuffer buf;
            BackupValue<bool> backupReportErrors(&m_ReportErrors);
            m_ReportErrors |= m_ReportMultilineLambdaReturnTypeInferenceErrors.HasValue() && m_ReportMultilineLambdaReturnTypeInferenceErrors.GetValue() && m_Errors != NULL;
            ReportSemanticError(WRNID_ObjectAssumed1, pLiteral->Loc, ResLoadString(WRNID_ArrayInitTooManyTypesObjectAssumed, &buf));
        }

    }

    else if (pTargetType->IsArrayType() && 
        (pArrayLiteralType->GetRank() == pTargetType->PArrayType()->GetRank() || IsEmptyArrayLiteralType(pArrayLiteralType)))
    {
        // If converting {e0,e1} -> T(), then {e0,e1} needs to become a T() as well.
        pIntermediateElementType = pTargetType->PArrayType()->GetRoot();
        IntermediateRank = pTargetType->PArrayType()->GetRank();
        NeedsCoerce = false;
    }

    else if (pArrayLiteralType->GetRank() == 1 && pTargetType->IsGenericBinding() && 
             ((GetCompilerHost()->GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIListType) &&
               (pTargetType->PGenericBinding()->GetGeneric() == GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::GenericIEnumerableType) ||
                pTargetType->PGenericBinding()->GetGeneric() == GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::GenericICollectionType) ||
                pTargetType->PGenericBinding()->GetGeneric() == GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::GenericIListType))) ||
              (GetCompilerHost()->GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIReadOnlyListType) &&
               (pTargetType->PGenericBinding()->GetGeneric() == GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::GenericIReadOnlyListType) ||
                pTargetType->PGenericBinding()->GetGeneric() == GetCompilerHost()->GetFXSymbolProvider()->GetType(FX::GenericIReadOnlyCollectionType)))))
               
    {
        // If converting {e0,e1} -> IEnumerable(Of T), then {e0,e1} needs to become a T()
        pIntermediateElementType = pTargetType->PGenericBinding()->GetArgument(0);
        IntermediateRank = 1;
        NeedsCoerce = true;
    }

    else if(TypeHelpers::IsStringType(pTargetType) && TypeHelpers::IsCharArrayRankOne(pArrayLiteralType))
    {
        // This is added for 
        pIntermediateElementType = pArrayLiteralType->GetRoot();
        ILTree::ArrayLiteralExpression * pIntermediateArrayExpression = 
            AllocateArrayLiteralExpression(ConvertArrayLiteralElements(pLiteral, pIntermediateElementType,
                                                                        RequiresNarrowingConversion,
                                                                        NarrowingFromNumericLiteral),
                                           pLiteral->Rank,
                                           pLiteral->Dims,
                                           pLiteral->Loc);
        pIntermediateArrayExpression->ResultType = pArrayLiteralType;
        return AllocateExpression(SX_CTYPE,                          
                                  pTargetType, 
                                  pIntermediateArrayExpression, 
                                  pLiteral->Loc);
    }
    
    else
    {
        // Otherwise there were no reclassifications that matched the shape of the array
        // literal to its intended target.
        VSASSERT(pTargetType!=NULL, "oops! we promised never to return NULL if given a NULL target type");
        return NULL;
    }


    if (pIntermediateElementType==NULL || pIntermediateElementType->IsVoidType())
    {
        VSFAIL("Was asked to convert an array literal to a void type. That doesn't even make sense.");
        ReportSemanticError(ERRID_InternalCompilerError, pLiteral->Loc);
        return MakeBad(pLiteral);
    }



    // Now we judge what we have to do for the appropriate intermediate array expression.

    ILTree::ArrayLiteralExpression *pIntermediateArrayExpression;

    if (pArrayLiteralType->GetRank() == IntermediateRank)
    {  
        // Here we have to go back to the elements and convert them
        pIntermediateArrayExpression = AllocateArrayLiteralExpression(
                                            ConvertArrayLiteralElements(pLiteral, pIntermediateElementType,
                                                                        RequiresNarrowingConversion,
                                                                        NarrowingFromNumericLiteral),
                                            pLiteral->Rank,
                                            pLiteral->Dims,
                                            pLiteral->Loc);
    }

    else if (IsEmptyArrayLiteralType(pArrayLiteralType))
    {
        unsigned * pDims = m_SymbolCreator.GetNorlsAllocator()->AllocArray<unsigned>(IntermediateRank);
        pIntermediateArrayExpression = AllocateArrayLiteralExpression(
                                            NULL,
                                            IntermediateRank,
                                            pDims,
                                            pLiteral->Loc);
    }

    else
    {
        VSFAIL("internal logic error: we should have covered all the possibilities that were allowed for above");
        ReportSemanticError(ERRID_InternalCompilerError, pLiteral->Loc);
        return NULL;
    }

    // Here's where we erase the reclassifiability from the type.
    // Incidentally, it might be that pIntermediateArrayExpression itself is still an SX_ARRAYLITERALType;
    // the erasure is indicated solely by ResultType
    pIntermediateArrayExpression->ResultType = m_SymbolCreator.GetArrayType(IntermediateRank, pIntermediateElementType);

    // Finally, coerce if necessary
    VSASSERT(NeedsCoerce || pTargetType==NULL || BCSYM::AreTypesEqual(pIntermediateArrayExpression->ResultType, pTargetType), "if coercion was needed from array literal then why doesn't the array literal have the right type?");
    if (NeedsCoerce)
    {
        return AllocateExpression( SX_WIDE_COERCE, pTargetType, pIntermediateArrayExpression, pIntermediateArrayExpression->Loc);
    }
    else
    {
        return pIntermediateArrayExpression;
    }
}


ILTree::Expression *
Semantics::ConvertWithErrorChecking
(
    ILTree::Expression *Input,
    Type *TargetType,
    ExpressionFlags Flags,
    Parameter *CopyBackConversionParam,  // This argument is not NULL when conversion occurs in the context of the copyback of an argument passed ByRef.
    bool &RequiresNarrowingConversion,
    bool &NarrowingFromNumericLiteral,
    bool SuppressMethodNameInErrorMessages,
    DelegateRelaxationLevel &delegateRelaxationLevel,
    bool & RequiresUnwrappingNullable,
    AsyncSubAmbiguityFlags *pAsyncSubArgumentAmbiguity,
    bool IgnoreOperatorMethod
)
{
    VSASSERT(!IsBad(Input), "Bad expression surprise.");

    if (TargetType!=NULL && TypeHelpers::IsBadType(TargetType))
    {
        return MakeBad(Input);
    }

    RequiresUnwrappingNullable = false;

    // If given "NULL" TargetType, then this function classifies its input expression as a value
    // in a context where the target type is unknown. If given a concrete TargetType, then it classifies
    // its input in a context where the target type is known.
    //
    // The difference is manifest in things like {1,"hello"}. In a context where the target type is known
    // to be Object(), then it produces an object array without problem. But in a context where the
    // target type is unknown, then it produces the same object array but might additionally give a
    // warning/error about "Object Assumed". Likewise with unbound lambdas.
    //
    // With respect to this difference, special treatment is given for unbound lambdas, array literals,
    // Nothing literals and AddressOf.
   
    if (TargetType == NULL &&
        Input->bilop != SX_UNBOUND_LAMBDA &&
        Input->bilop != SX_ADDRESSOF &&
        Input->bilop != SX_NOTHING &&
        Input->bilop != SX_ARRAYLITERAL)
    {
        return Input;
    }



    // check boxing of restricted types
    if (Input->vtype == t_struct && 
        m_ReportErrors &&
        TargetType!=NULL &&
        (TypeHelpers::IsRootValueType(TargetType, m_CompilerHost) || TargetType == GetFXSymbolProvider()->GetObjectType()))
    {
        CheckRestrictedType(
            ERRID_RestrictedConversion1,
            Input->ResultType->DigThroughAlias(),
            &Input->Loc,
            m_CompilerHost,
            m_Errors);
    }

    // Convert expressions to an expression tree
    if( TargetType!=NULL && IsConvertibleToExpressionTree( TargetType, Input ) )
    {
        // this is safe because we wouldn't be in here if GetFXSymbolProvider()->GetType(FX::GenericExpressionType)
        // didn't succeed in  IsConvertibleToExpressionTree(), above.

        VSASSERT( TypeHelpers::IsGenericTypeBinding( TargetType ) &&
            TypeHelpers::EquivalentTypes( TargetType->PGenericTypeBinding()->GetGenericType(),
                GetFXSymbolProvider()->GetType(FX::GenericExpressionType)
                ),
            "Must be an Expression(Of T)!"
            );

        // We've got an Expression(Of T), convert the expression to T first

        Input =
            ConvertWithErrorChecking
            (
                Input,
                TargetType->PGenericTypeBinding()->GetArgument(0),
                Flags,
                NULL,
                RequiresNarrowingConversion,
                NarrowingFromNumericLiteral,
                SuppressMethodNameInErrorMessages,
                delegateRelaxationLevel,
                RequiresUnwrappingNullable,
                pAsyncSubArgumentAmbiguity
            );

        // We will always allow a conversion to an expression tree here so that overload resolution
        // will not reject methods that take an Expression(Of T).

        return AllocateExpression( SX_WIDE_COERCE, TargetType, Input, Input->Loc );
    }


    // EXPRESSION RECLASSIFICATION IS CHECKED FIRST.
    // Things like ArrayLiteral -> actual array, unbound lambda -> bound lambda, AddressOf -> choice of method.
    // These reclassifications cannot be left until later, because the code that comes later
    // might use DirectCast semantics (i.e. ClassifyPredefinedCLRConversion) which wouldn't even
    // apply to these things.
    //
    // BUT...
    // These checks here are only for the immediate conversions listed above.
    // They don't encompass user-defined conversions.
    // That's what we want: things like DirectCast(e, T) should *not* be using user-defined conversions.
    // It's only when we're asked for regular conversion semantics, and we fall through to ClassifyConversion,
    // that we'll look for user-defined conversions in conjunction with array-literal -> actual array.


    // Turn an array literal into a concrete array (or perhaps an IEnumerable(Of T) or the like)
    if (Input->ResultType->IsArrayLiteralType())
    {
        // Note: do NOT disable this assertion. If you have ArrayLiteralTypes which aren't SX_ARRAYLITERALS
        // then the Convert* routines will fail in mysterious ways, as will InterpretExpresion, and ClassifyConversion.
        VSASSERT(Input->bilop==SX_ARRAYLITERAL, "unexpected: a non-array-literal expression which claims to have a literal type");
    }
    if (Input->ResultType->IsArrayLiteralType() && Input->bilop==SX_ARRAYLITERAL)
    {
        bool _RequiresNarrowingConversion = false;
        bool _NarrowingFromNumericLiteral = false;

        ILTree::Expression * pResult = ConvertArrayLiteral(&Input->AsArrayLiteralExpression(), TargetType,
                        _RequiresNarrowingConversion,
                        _NarrowingFromNumericLiteral
                    );

        if (pResult!=NULL)
        {
            RequiresNarrowingConversion = _RequiresNarrowingConversion;
            NarrowingFromNumericLiteral = _NarrowingFromNumericLiteral;
            
            return pResult;
        }
        // The decision about whom handles which conversion is subtle. The above routine looks for
        // "matching shapes": if it finds {e0,e1}->T() or {{e0},{e1}}->T(,) or the like then it claims to have
        // succeeded, and goes ahead with conversions e0->T and e1->T, and reports any errors on them if they fail.
        // It also claims to have succeeded with {e0}->IEnumerable/ICollection/IList, and with {}/{,}->System.Array/Object.
        // What's interesting about all these cases is that user-defined conversions would never have helped.
        // (User-defined conversions only ever apply if the source or target was user-defined).
        // That's why the function could definitively take ownership of those particular matching shapes.
        //
        // NB. The above function handles TargetType==NULL by converting the array literal to its dominant
        // type (maybe giving warnings/errors in the process if appropriate, e.g. if the dominant type
        // doesn't exist or was assumed to be Object).
        //
        // Incidentally, if the shapes don't match, that doesn't RULE OUT a conversion. There might still
        // be conversions through user-defined operators. For instance,
        // "{new A} -> C" where A->B is user-defined and B()->C is user-defined.
        // They will be discovered later. (and only if not using DirectCast/TryCast semantics of course).
        //
        // ... but they'll never be discovered if the array literal had "void" type,
        // e.g. {AddressOf Main} in a context where the target type doesn't help, can never be solved.
        // And we'd rather give an error here "No type could be inferred" than an error later "array of void
        // cannot be converted to target type".
        if (!IsBad(Input) && TypeHelpers::IsVoidArrayLiteralType(Input->ResultType))
        {
            // "A type could not be inferred from the elements of the array. Specifying the type of the array might correct this error."
            ReportSemanticError(ERRID_ArrayInitNoType, Input->ResultType->PArrayLiteralType()->GetLiteralLocation());
            return MakeBad(Input);
        }
    }

    // Turn an AddressOf expression into a delegate binding, or a pure lambda into a lambda.
    // It also works with the target type is a plain System.Delegate or System.MultiCastDelegate
    if (Input->bilop == SX_ADDRESSOF || 
        Input->bilop == SX_UNBOUND_LAMBDA || 
        (Input->bilop == SX_LAMBDA && !Input->AsLambdaExpression().IsExplicitlyConverted))
    {
        // If you change this condition, make sure an adjustment of delegateRelaxationLevel
        // below (Dev10 #626389) still catches all scenarios we care about.
        if (TargetType == NULL ||
            TypeHelpers::IsDelegateType(TargetType) ||
            TargetType == GetFXSymbolProvider()->GetObjectType() ||
            TargetType == GetFXSymbolProvider()->GetDelegateType())
        {
            // Delegate binding.
            ILTree::Expression *Binding;

            if (Input->bilop == SX_ADDRESSOF)
            {
                // SX_ADDRESSOF does not convert to the following types.

                if (TargetType == NULL ||
                    TypeHelpers::IsStrictSupertypeOfConcreteDelegate(TargetType, GetFXSymbolProvider())) // covers Object, System.Delegate, System.MulticastDelegate
                {
                    goto NotCreatableDelegateType;
                }
                else
                {

                    //

                    // We consider a relaxation as a narrowing conversion to assist in overload resolution
                    // and prevent applications that compiled in VB8 to fail in VB9 because there are more matches

                    VSASSERT(TargetType!=NULL, "internal logic error: TargetType should not be NULL here");

                    Binding = InterpretDelegateBinding
                    (
                        Input,
                        TargetType,
                        Input->Loc,
                        SuppressMethodNameInErrorMessages,
                        Flags,
                        delegateRelaxationLevel,
                        &RequiresNarrowingConversion
                    );

                }
            }
            else if (Input->bilop == SX_UNBOUND_LAMBDA)
            {
                // Note:
                // InterpretUnboundLambdaBinding, when given Object, System.Delegate,
                // or System.MulticastDelegate, will bind the lambda body and then
                // relax it. But the function doen't set the "RelaxationLevel" flag in all cases,
                // so we do it.

                // Note: TargetType==NULL means "report any errors"
                BackupValue<TriState<bool>> backup_report_type_inference_errors(&m_ReportMultilineLambdaReturnTypeInferenceErrors);
                if (TargetType == NULL)
                {
                    m_ReportMultilineLambdaReturnTypeInferenceErrors.SetValue(true);
                }

                // Following function handles TargetType==NULL by inferring the type of the lambda (which
                // will be an anonymous delegate) and reclassifying the lambda as that type.
                // And it handles TargetType!=NULL in the obvious way.
                bool DroppedAsyncReturnTask = false;

                Binding = InterpretUnboundLambdaBinding
                (
                    &Input->AsUnboundLambdaExpression(),
                    TargetType,
                    true, // ConvertToDelegateReturnType
                    delegateRelaxationLevel,
                    false, // InferringReturnType
                    &RequiresNarrowingConversion,
                    &NarrowingFromNumericLiteral,
                    (Flags & ExprGetLambdaReturnTypeFromDelegate),
                    false,
                    pAsyncSubArgumentAmbiguity,
                    &DroppedAsyncReturnTask
                );

                // MQ 


                if (Binding->bilop == SX_LAMBDA && HasFlag(Flags, ( ExprCreateDelegateInstance | ExprIsExplicitCast)))
                {
                    Binding->AsLambdaExpression().IsExplicitlyConverted = true;
                }
                
                // If we dropped the return task, warn, e.g. "Dim x As Action = Async Function() 1" (except if explicitly casted)
                if (DroppedAsyncReturnTask && !HasFlag(Flags, ( ExprCreateDelegateInstance | ExprIsExplicitCast) ))
                {
                    ReportSemanticError(WRNID_UnobservedAwaitableDelegate, ExtractLambdaErrorSpan(&Input->AsUnboundLambdaExpression()));
                }

            }
            else
            {
                AssertIfFalse(Input->bilop == SX_LAMBDA);
                if (TypeHelpers::IsDelegateType(TargetType) || Input->AsLambdaExpression().IsPartOfQuery)
                {
                    // SX_ADDRESSOF does not convert to the following types.

                    if (TypeHelpers::IsStrictSupertypeOfConcreteDelegate(TargetType, GetFXSymbolProvider())) // covers Object, System.Delegate, System.MulticastDelegate
                    {
                        goto NotCreatableDelegateType;
                    }
                    // Port SP1 CL 2939886 to VS10.
                    // 

                    // Some varification for 























                    
                    else if (
                        (Input->ResultType != NULL && 
                            !TypeHelpers::IsVoidType(Input->ResultType) && 
                            !Input->AsLambdaExpression().IsPartOfQuery) ||
                        Input->AsLambdaExpression().IsStatementLambda)                  
                    {
                        Binding = ConvertAnonymousDelegateToOtherDelegate(Input, TargetType);

                        // MQ 


                        if (Binding->bilop == SX_LAMBDA && HasFlag(Flags, ( ExprCreateDelegateInstance | ExprIsExplicitCast)))
                        {
                            Binding->AsLambdaExpression().IsExplicitlyConverted = true;
                        }
                            
                        return Binding;
                        // WARNING: Dev10#691597 -- this function does something quite different from what its name says.
                        // (1) If Input is a named (non-anonymous delegate) that's already of the same type as TargetType
                        //     then it just returns it
                        // (2) Otherwise, regardless of whether Input is named or anonymous, it constructs an
                        //     AddressOf expression "AddressOf <Input>.Invoke" and works on that.
                    }
                    else
                    {
                        // ConvertToDelegateType is a "bad" function because essentially, it takes a lambda that
                        // we've bound, and rewrites the single line body with a conversion, and changes the lambda return
                        // type. It is not aware of multiline lambdas. But we need it for queries, which only builds single
                        // line lambdas, which is why we have an assert here.

                        // 



















                        //VSASSERT(Input->AsLambdaExpression().IsPartOfQuery, "This lambda should be part of a query, and it is not. This requires investigation. See 
                        Binding = ConvertToDelegateType( &Input->AsLambdaExpression(), TargetType, true, delegateRelaxationLevel, 
                                                         &RequiresNarrowingConversion, &NarrowingFromNumericLiteral );
                        
                        // MQ 


                        if (Binding->bilop == SX_LAMBDA && HasFlag(Flags, ( ExprCreateDelegateInstance | ExprIsExplicitCast)))
                        {
                            Binding->AsLambdaExpression().IsExplicitlyConverted = true;
                        }
                    }
                }
                else
                {
                    Binding = Input;
                }
            }

            if (IsBad(Binding))
            {
                return Binding;
            }

            Input = Binding;
        }
        else
        {
NotCreatableDelegateType:
            Location loc = (Input->bilop == SX_UNBOUND_LAMBDA && Input->AsUnboundLambdaExpression().IsStatementLambda)
                ? Input->AsUnboundLambdaExpression().GetLambdaStatement()->TextSpan
                : Input->Loc;

            if (TargetType == NULL)
            {
                // "Expression does not produce a value"
                ReportSemanticError(ERRID_VoidValue, loc);
            }
            else if (TargetType == GetFXSymbolProvider()->GetDelegateType() || TargetType == GetFXSymbolProvider()->GetMultiCastDelegateType())
            {
                // "AddressOf/Lambda cannot be converted to '|1' because '|1' is declared MustInherit and cannot be created"
                ReportSemanticError((Input->bilop==SX_ADDRESSOF) ? ERRID_AddressOfNotCreatableDelegate1 : ERRID_LambdaNotCreatableDelegate1, loc, TargetType);
            }
            else
            {
                // "AddressOf/Lambda cannot be converted to '|1' because '|1' is not a delegate type"
                ReportSemanticError((Input->bilop==SX_ADDRESSOF) ? ERRID_AddressOfNotDelegate1 : ERRID_LambdaNotDelegate1, loc, TargetType);
            }

            return MakeBad(Input);
        }
    }

    Input = MakeRValue(Input,TargetType);

    if (IsBad(Input) || TargetType==NULL)
    {
        return Input;
    }

    VSASSERT(
        Input->bilop != SX_LATE_REFERENCE &&
            Input->bilop != SX_PROPERTY_REFERENCE &&
            !HasFlag32(Input, SXF_LVALUE),
        "Attempted conversion on non-Rvalue.");

    Type *SourceType = Input->ResultType;

    Procedure *OperatorMethod = NULL;
    GenericBinding *OperatorMethodGenericContext = NULL;
    bool OperatorMethodIsLifted;
    unsigned XmlLiteralErrorId = 0;

    bool ConversionIsNarrowingDueToAmbiguity = false;

    ConversionClass ConversionClassification;
    //
    if (HasFlag(Flags, ExprHasDirectCastSemantics))
    {
        ConversionClassification = ClassifyPredefinedCLRConversion(
                                        TargetType,
                                        SourceType,
                                        ConversionSemantics::Default,
                                        false,
                                        NULL,
                                        NULL,
                                        &ConversionIsNarrowingDueToAmbiguity);
    }
    else if (HasFlag(Flags, ExprHasTryCastSemantics))
    {
        ConversionClassification = ClassifyTryCastConversion(
                                        TargetType,
                                        SourceType);
    }
    else
    {
        // Dev10#703328: the case of passing a lambda to a function has already been dealt with above, e.g. "g(Function() 1)".
        // But if we're passing a VB$AnonymousDelegate e.g. "Dim f = Function() 1 : g(f)" then it falls through to here. And
        // we want this one to set delegateRelaxationLevel just like the lambda case did above.
        DelegateRelaxationLevel conversionRelaxationLevel = DelegateRelaxationLevelNone;

        ConversionClassification = ClassifyConversion(
                                        TargetType,
                                        SourceType, 
                                        OperatorMethod, 
                                        OperatorMethodGenericContext, 
                                        OperatorMethodIsLifted, 
                                        true, 
                                        &RequiresUnwrappingNullable,
                                        &ConversionIsNarrowingDueToAmbiguity,
                                        &conversionRelaxationLevel,
                                        IgnoreOperatorMethod);

        delegateRelaxationLevel = max(delegateRelaxationLevel, conversionRelaxationLevel);
    }

    if ( ConversionClassification == ConversionNarrowing &&
         (
            ( TypeHelpers::IsInterfaceType( SourceType ) &&
              (TypeHelpers::IsClassType(TargetType) && TargetType->PClass()->IsNotInheritable() && !TargetType->IsComImportClass() ))
            ||
            ( TypeHelpers::IsInterfaceType( TargetType ) &&
              (TypeHelpers::IsClassType(SourceType) && SourceType->PClass()->IsNotInheritable() && !SourceType->IsComImportClass() ))
         )
       )
    {
        // Microsoft: ClassifyPredefinedCLRConversion will return ConversionNarrowing if SourceType is an
        // interface type and TargetType is a class type (or vice versa). We want to do the following:
        // 1. If this is XML scenario, generate an error.
        // 2. Otherwise, leave it as a narrowing conversion, but emit a warning.

        Type* ClassType = NULL;
        Type* InterfaceType = NULL;

        ClassType = TypeHelpers::IsClassType(TargetType) ? TargetType : SourceType;
        InterfaceType = TypeHelpers::IsInterfaceType(TargetType) ? TargetType : SourceType;

        VSASSERT( ClassType != InterfaceType, "Code defect here." );

        if( !TypeHelpers::Implements(
                ClassType,
                InterfaceType,
                m_SymbolCreator,
                false,
                NULL,
                m_CompilerHost,
                false,
                NULL,
                NULL) )
        {
            // Ok, the target type does not implement the source type. So we must
            // check the conditions above.

            if( GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIEnumerableType) &&
                m_XmlSymbols.GetXElement() &&
                TypeHelpers::IsCompatibleWithGenericEnumerableType(
                    InterfaceType,
                    m_XmlSymbols.GetXElement(),
                    m_SymbolCreator,
                    m_CompilerHost
                    )
              )
            {
                XmlLiteralErrorId = ERRID_UseValueForXmlExpression3;
                ConversionClassification = ConversionError;
            }
            else
            {
                if( m_ReportErrors )
                {
                    ReportSemanticError(
                        WRNID_InterfaceConversion2,
                        Input->Loc,
                        SourceType,
                        TargetType
                        );
                }
            }
        }
    }

    // We also want to report an XML error if we have a conversion error and the source type
    // is an IEnumerable(Of XElement).

    if( ConversionClassification == ConversionError &&
        TypeHelpers::IsInterfaceType( SourceType ) &&
        TypeHelpers::IsValueType( TargetType ) &&
        GetFXSymbolProvider()->IsTypeAvailable(FX::GenericIEnumerableType) &&
        m_XmlSymbols.GetXElement() &&
        TypeHelpers::IsCompatibleWithGenericEnumerableType(
            SourceType,
            m_XmlSymbols.GetXElement(),
            m_SymbolCreator,
            m_CompilerHost
            )
      )
    {
        XmlLiteralErrorId = ERRID_TypeMismatchForXml3;
    }

    if (HasFlag(Flags, ExprIsOperandOfConditionalBranch) &&
        (ConversionClassification == ConversionError ||
            ConversionClassification == ConversionNarrowing) &&
        CanTypeContainUserDefinedOperators(SourceType))
    {
        Type * pNullableBoolType = m_SymbolCreator.LiftType(GetFXSymbolProvider()->GetBooleanType(), m_CompilerHost);
        Procedure * OperatorMethod2 = NULL;
        GenericBinding * OperatorMethodGenericContext2 = NULL;
        bool OperatorMethodIsLifted2 = false;

        bool RequiresUnwrappingNullable2 = false;

        ConversionClass ConversionClassification2 =
            HasFlag(Flags, ExprHasDirectCastSemantics) ?
                ClassifyPredefinedCLRConversion(pNullableBoolType, SourceType, ConversionSemantics::Default) :
                    (HasFlag(Flags, ExprHasTryCastSemantics) ?
                        ClassifyTryCastConversion(pNullableBoolType, SourceType) :
                        ClassifyConversion(pNullableBoolType, SourceType, OperatorMethod2, OperatorMethodGenericContext2, OperatorMethodIsLifted2, false, &RequiresUnwrappingNullable2));
        if
        (
            GetFXSymbolProvider()->GetBooleanType() &&
            TargetType == GetFXSymbolProvider()->GetBooleanType() &&
            TypeHelpers::IsNullableType(SourceType, m_CompilerHost) &&
            BCSYM::AreTypesEqual
            (
                GetFXSymbolProvider()->GetBooleanType(),
                TypeHelpers::GetElementTypeOfNullable(SourceType, m_CompilerHost)
            )
        )
        {
            RequiresUnwrappingNullable = false;

            //This does not make a method uncallable, because the conversion is successfull
            return
                AllocateExpression
                (
                    SX_ISTRUE,
                    TargetType,
                    Input,
                    Input->Loc
                );
        }

        // Conversion to Boolean in the context of a conditional branch can resolve to a user-defined
        // operator IsTrue. Given source type S, the conversion proceeds in four steps:
        //
        // 1) If S widens to Boolean, perform the widening conversion.
        // 2) Otherwise, if S defines S::IsTrue(S) As Boolean, call the IsTrue operator.
        // 3) Otherwise, if S narrows to Boolean, perform the narrowing conversion.
        // 4) Otherwise, the conversion is not possible.
        //
        // This code performs step 2. If a valid IsTrue operator exists, call it.

        if
        (
            ConversionClassification2 == ConversionWidening
        )
        {
            RequiresUnwrappingNullable = RequiresUnwrappingNullable2;

            return
                AllocateExpression
                (
                    SX_ISTRUE,
                    TargetType,
                    ConvertWithErrorChecking
                    (
                        Input,
                        pNullableBoolType,
                        Flags & ~(ExprIsOperandOfConditionalBranch), //Mask out the ExprIsOperandOfConditionalBranch...
                        false,
                        NULL
                    ),
                    Input->Loc
                );
        }
        else
        {
            VSASSERT
            (
                TypeHelpers::EquivalentTypes(GetFXSymbolProvider()->GetBooleanType(), TargetType) ||
                TypeHelpers::EquivalentTypes(pNullableBoolType, TargetType),
                "expected boolean for conditional branch"
            );

            Procedure *ConditionalOperatorMethod = NULL;
            GenericBinding *ConditionalOperatorMethodGenericContext = NULL;

            bool ResolutionFailed = false;
            bool ResolutionIsLateBound = false;
            bool PreviouslyReportingErrors = m_ReportErrors;
            m_ReportErrors = false;

            //EXTMET - 
            Type *OperatorResultType =
                ResolveUserDefinedOperator(
                    SX_ORELSE,   // 
                    Input->Loc,
                    Input,
                    ResolutionFailed,
                    ResolutionIsLateBound,
                    ConditionalOperatorMethod,
                    ConditionalOperatorMethodGenericContext);

            m_ReportErrors = PreviouslyReportingErrors;


            if
            (
                ConditionalOperatorMethod &&
                !ResolutionFailed &&
                !ResolutionIsLateBound
            )
            {
                if
                (
                    TypeHelpers::EquivalentTypes(OperatorResultType, GetFXSymbolProvider()->GetBooleanType()) ||
                    (
                        TypeHelpers::IsNullableType(OperatorResultType, m_CompilerHost) &&
                        BCSYM::AreTypesEqual(
                            TypeHelpers::GetElementTypeOfNullable(OperatorResultType, m_CompilerHost),
                            GetFXSymbolProvider()->GetBooleanType())
                    )
                )
                {

                    //EXTMET - 


                    ILTree::Expression * Result =
                        InterpretUserDefinedOperator
                        (
                            SX_COUNT,
                            ConditionalOperatorMethod,
                            ConditionalOperatorMethodGenericContext,
                            Input->Loc,
                            Input,
                            Flags
                        );

                    if (TypeHelpers::IsNullableType(OperatorResultType, m_CompilerHost))
                    {
                        Result =
                            ConvertWithErrorChecking
                            (
                                Result,
                                TargetType,
                                Flags,
                                SuppressMethodNameInErrorMessages,
                                &RequiresUnwrappingNullable2
                            );

                        RequiresUnwrappingNullable = RequiresUnwrappingNullable || RequiresUnwrappingNullable2;
                    }

                    return Result;
                }
            }
        }

        if
        (
            ConversionClassification2 == ConversionNarrowing &&
            !BCSYM::AreTypesEqual
            (
                SourceType,
                GetFXSymbolProvider()->GetObjectType()
            )
        )
        {
            RequiresUnwrappingNullable = RequiresUnwrappingNullable2;

            return
                AllocateExpression
                (
                    SX_ISTRUE,
                    TargetType,
                    ConvertWithErrorChecking
                    (
                        Input,
                        pNullableBoolType,
                        Flags & ~(ExprIsOperandOfConditionalBranch), //Mask out the ExprIsOperandOfConditionalBranch...
                        false,
                        NULL
                    ),
                    Input->Loc
                );
       }

    }

    if (ConversionClassification == ConversionError && !HasFlag(Flags, ExprHasExplicitCastSemantics) &&
        MakeVarianceConversionSuggestion(Input, TargetType, ConversionClassification))
    {
        VSASSERT(IsBad(Input), "If the conversion was an error, and there was a variance suggestion, then MakeVarianceConversionSuggestion should have marked it bad.");
        return MakeBad(Input);
    }
        
    if (ConversionClassification == ConversionNarrowing)
    {
        // Because the static type of "Nothing" is Object,
        // converting it to any type appears to be narrowing, when
        // in fact it is widening.

        // A literal 0 widens to any Enum type.

        if ((IsNothingLiteral(Input) && !Input->IsExplicitlyCast) ||
            (IsIntegerZeroLiteral(Input) && TypeHelpers::IsEnumType(TargetType)))
        {
            ConversionClassification = ConversionWidening;
        }
        else if (!HasFlag(Flags, ExprHasExplicitCastSemantics))
        {
            bool CandidateForReclassification = true;
            Type *ReclassificationType = TargetType;

            if (OperatorMethod)
            {
                Type *IntermediateTargetType = TypeInGenericContext(OperatorMethod->GetType(), OperatorMethodGenericContext);

                CandidateForReclassification =
                    OperatorMethod->GetAssociatedOperatorDef()->GetOperator() == OperatorWiden &&
                    ClassifyPredefinedConversion(TargetType, IntermediateTargetType) != ConversionNarrowing;
                ReclassificationType = TypeInGenericContext(OperatorMethod->GetFirstParam()->GetType(), OperatorMethodGenericContext);
            }

            // Narrowing of constant integral and floating values is permitted if the value fits in
            // the target type, regardless of strictness.
            if (CandidateForReclassification &&
                (Input->bilop == SX_CNS_INT &&
                 ((TypeHelpers::IsIntegralType(ReclassificationType) && !TypeHelpers::IsEnumType(ReclassificationType)) &&
                  !(TypeHelpers::IsEnumType(SourceType) || TypeHelpers::IsBooleanType(SourceType) || TypeHelpers::IsCharType(SourceType)))) ||
                (Input->bilop == SX_CNS_FLT && TypeHelpers::IsFloatingType(ReclassificationType)))
            {
                NarrowingFromNumericLiteral = true;
            }
            else
            {
                Type * ReclassificationElementType = TypeHelpers::GetElementTypeOfNullable(ReclassificationType, m_CompilerHost);

                if
                (
                    TypeHelpers::IsNullableType(ReclassificationType, m_CompilerHost) &&
                    CandidateForReclassification &&
                    (
                        (
                            Input->bilop == SX_CNS_INT &&
                            TypeHelpers::IsIntegralType(ReclassificationElementType) &&
                            !TypeHelpers::IsEnumType(SourceType) &&
                            !TypeHelpers::IsBooleanType(SourceType) &&
                            !TypeHelpers::IsCharType(SourceType)
                        ) ||
                        (
                            Input->bilop == SX_CNS_FLT &&
                            TypeHelpers::IsFloatingType(ReclassificationElementType)
                        )
                    )
                )
                {
                    NarrowingFromNumericLiteral = true;
                }
                else if (CopyBackConversionParam)
                {
                    if ( m_UsingOptionTypeStrict )
                    {
                        ReportSemanticError(
                            ERRID_StrictArgumentCopyBackNarrowing3,
                            Input->Loc,
                            CopyBackConversionParam->GetName(),
                            SourceType,
                            TargetType);
                    }
                    else if (WarnOptionStrict())
                    {
                        // warning
                        ReportSemanticError(
                            WRNID_ImplicitConversionCopyBack,
                            Input->Loc,
                            CopyBackConversionParam->GetName(),
                            SourceType,
                            TargetType);
                    }
                }
                else if (MakeVarianceConversionSuggestion(Input, TargetType, ConversionClassification))
                {    
                    if (IsBad(Input))
                    {
                        return Input;
                    }
                }
                else
                {
                    // We have a narrowing conversion. This is how we might display it, depending on context:
                    // ERRID_NarrowingConversionDisallowed2 "Option Strict On disallows implicit conversions from '|1' to '|2'."
                    // ERRID_NarrowingConversionCollection2 "Option Strict On disallows implicit conversions from '|1' to '|2'; the Visual Basic 6.0 collection type is not compatible with the .NET Framework collection type."
                    // ERRID_AmbiguousCastConversion2       "Option Strict On disallows implicit conversions from '|1' to '|2' because the conversion is ambiguous."
                    // The Collection error is for when one type is Microsoft.VisualBasic.Collection and
                    // the other type is named _Collection.
                    // The Ambiguous error is for when the conversion was classed as "Narrowing" for reasons of ambiguity.
                    unsigned ErrorId = m_UsingOptionTypeStrict ? ERRID_NarrowingConversionDisallowed2 :
                        (WarnOptionStrict() ? WRNID_ImplicitConversion2 : 0);
                    unsigned int SubstErrorInto = (ErrorId == WRNID_ImplicitConversion2) ? WRNID_ImplicitConversionSubst1 : 0;
                    // substitution... the "WRNID_ImplicitConversion2" is just a string which gets substituted
                    // into WRNID_ImplicitConversionSubst1, BC42016. Most implicit conversions get the number 42016.

                    if (m_UsingOptionTypeStrict && SourceType->IsNamedRoot() && TargetType->IsNamedRoot())
                    {
                        // Check for Microsoft.VisualBasic.Collection
                        if ((m_CompilerHost->IsRuntimeType(STRING_CONST(m_Compiler, Collection), SourceType->PNamedRoot())) ||
                            (m_CompilerHost->IsRuntimeType(STRING_CONST(m_Compiler, Collection), TargetType->PNamedRoot())))
                        {
                            // Check for _Collection
                            if ((StringPool::IsEqual(STRING_CONST(m_Compiler, _Collection), SourceType->PNamedRoot()->GetName())) ||
                                (StringPool::IsEqual(STRING_CONST(m_Compiler, _Collection), TargetType->PNamedRoot()->GetName())))
                            {
                                // Got both, so use the more specific error message
                                ErrorId = ERRID_NarrowingConversionCollection2;
                            }
                        }
                    }

                    if (ConversionIsNarrowingDueToAmbiguity)
                    {
                        // Ambiguity narrowing is a case of implicit conversions. So we use the same logic
                        // for deciding whether ambiguity narrowing (like other implicit-conversion narrowing)
                        // should be counted as an error, a warning, or not at all:
                        ErrorId = m_UsingOptionTypeStrict ? ERRID_AmbiguousCastConversion2 :
                            (WarnOptionStrict() ? WRNID_AmbiguousCastConversion2 : 0);
                        SubstErrorInto = (ErrorId == WRNID_AmbiguousCastConversion2) ? WRNID_ImplicitConversionSubst1 : 0;
                    }

                    if (ErrorId )
                    {
                        const WCHAR *Extra = NULL;

#if IDE
                        StringBuffer ExtraBuffer;

                        // Generate the type name and store it in the extra error field
                        ExtraBuffer.AppendSTRING(TargetType->ChaseToType()->GetGlobalQualifiedName());
                        BCSYM::FillInArray(m_Compiler, TargetType, &ExtraBuffer);

                        Extra = ExtraBuffer.GetString();
#endif

                        if (SubstErrorInto == 0)
                        {
                            // If no substitution is needed, then we just report the error straight:
                            ReportSemanticError(ErrorId, Extra, Input->Loc, SourceType, TargetType);
                        }
                        else
                        {
                            if (m_ReportErrors)
                            {
                                // ReportSemanticError is protected against being called when m_ReportErrors is false,
                                // but ExtractErrorName isn't. Hence the guard above.
                                StringBuffer buf, buf1, buf2;
                                ResLoadStringRepl(ErrorId, &buf, ExtractErrorName(SourceType,buf1), ExtractErrorName(TargetType,buf2));
                                ReportSemanticError(SubstErrorInto, Extra, Input->Loc, buf.GetString());
                            }
                        }

                    }

                    // error only for Option Strict, otherwise just warning
                    if (m_UsingOptionTypeStrict)
                    {
                        return MakeBad(Input);
                    }
                }

            }

            RequiresNarrowingConversion = true;
        }
    }

    if (ConversionClassification != ConversionError)
    {
        ILTree::Expression *Result;

        if (OperatorMethod)
        {
            if ( OperatorMethodIsLifted )
            {
                // S?-->T? and there is a user defined op S-->T. Nullable lifted op. Delay it for null check in lowering phase.
                // Note: when only one of source(S) or target(T) is nullable, the user defined op S->T is still picked up by normal conversions.
                // It is not a lifted op. though, and no nul check is performed.
                // Because expression trees gen. have to dig into the tree to find the method. Move the whole
                // intermediate type conversions in the lowering phase and pass only the method in reference methodOpCall.
                //
                Result = AllocateExpression(
                    SX_CTYPEOP,
                    TargetType,
                    Input,
                    Input->Loc);
                Result->AsLiftedCTypeExpression().OperatorMethod = OperatorMethod;
                Result->AsLiftedCTypeExpression().OperatorMethodContext = OperatorMethodGenericContext;
                Result->AsLiftedCTypeExpression().InterpretationFlags = Flags;
            }
            else
            {
                Result = ConvertUsingConversionOperator(Input, TargetType, OperatorMethod, OperatorMethodGenericContext, Flags);
            }

            if (!IsBad(Result))
            {
                CheckObsolete(OperatorMethod->GetAssociatedOperatorDef(), Input->Loc);
                CheckRecursiveOperatorCall(OperatorMethod, Input->Loc);
            }
        }
        else
        {
            Result = Convert(Input, TargetType, Flags, ConversionClassification);
        }

        // Here we need to simply the condition
        //
        //  Original Condition
        //        (HasFlag(Flags, ExprMustBeConstant) || IsAppliedAttributeContext()) &&    
        //        !IsBad(Result) &&
        //        (!IsConstant(Result) || (!IsAppliedAttributeContext() && TypeHelpers::IsArrayType(Result->ResultType))) &&
        //        !(IsAppliedAttributeContext() && IsValidAttributeConstant(Result)))
        //
        //        A  = IsAppliedAttributeContext();
        //        B  = IsBad(Result)
        //        C  = IsConstant(Result)
        //        AC = IsValidAttributeConstant(Result)
        //        MC = HasFlag(Flags, ExprMustBeConstant)
        //        AT = TypeHelpers::IsArrayType(Result->ResultType)
        //
        //   Symbolic Representation of Original Condition     
        //             ( MC || A) && !B && (!C || (!A && AT)) && (!A || !AC) 
        //    
        //   When IsAppliedAttributeContext() = true
        //              A && ( MC || A) && !B && (!C || (!A && AT)) && (!A || !AC) 
        //        <=>   A && ( MC || A) && !B && (!C || (!A && AT)) &&        !AC
        //        <=>   A &&               !B && (!C || (!A && AT)) &&        !AC
        //        <=>   A &&               !B && (!C || (!A && AT)) &&        !AC
        //        <=>   A &&               !B &&  !C                &&        !AC
        //             C => AC (Assumption 1)
        //        <=>   A &&               !B &&                              !AC
        //   When IsAppliedAttributeContext() = false     
        //              !A && ( MC || A) && !B && (!C || (!A && AT)) && (!A || !AC) 
        //        <=>   !A && ( MC || A) && !B && (!C || (!A && AT)) 
        //        <=>   !A &&   MC       && !B && (!C ||        AT ) 
        //
        //  shiqic

        // Assumption 1
        VSASSERT(!IsConstant(Result) || IsValidAttributeConstant(Result), "Aasumption is broken, either fix the assumpton or verify the proof");

        if (IsAppliedAttributeContext() && !IsBad(Result) && !IsValidAttributeConstant(Result))
        {
            ReportSemanticError(
                ERRID_RequiredAttributeConstConversion2,                    
                Input->Loc,
                SourceType,
                TargetType);
            Result = MakeBad(Result);
        }
        else if( 
            !IsAppliedAttributeContext() && 
            HasFlag(Flags, ExprMustBeConstant) && 
            !IsBad(Result) &&   
            (!IsConstant(Result) || TypeHelpers::IsArrayType(Result->ResultType)))
        {
            ReportSemanticError(
                ERRID_RequiredConstConversion2,
                Input->Loc,
                SourceType,
                TargetType);
            Result = MakeBad(Result);
        }

        return Result;
    }

    // At this point it is known that the conversion is erroneous.
    if (m_ReportErrors)
    {
        CompilerProject *SourceTypeProject = NULL;
        CompilerProject *TargetTypeProject = NULL;

        ConversionClass MixedProjectClassification =
            HasFlag(Flags, ExprHasDirectCastSemantics) ?
                ClassifyPredefinedCLRConversion(
                    TargetType,
                    SourceType,
                    ConversionSemantics::Default,
                    true,   // ignore project equivalence
                    &TargetTypeProject,
                    &SourceTypeProject) :
                (HasFlag(Flags, ExprHasTryCastSemantics) ?
                    ClassifyTryCastConversion(
                        TargetType,
                        SourceType,
                        true,   // ignore project equivalence
                        &TargetTypeProject,
                        &SourceTypeProject) :
                    ClassifyPredefinedConversion(
                        TargetType,
                        SourceType,
                        true,   // ignore project equivalence
                        &TargetTypeProject,
                        &SourceTypeProject));

        if (MixedProjectClassification != ConversionError &&
            SourceTypeProject && SourceTypeProject != m_Project &&
            TargetTypeProject && TargetTypeProject != m_Project &&
            SourceTypeProject != TargetTypeProject)
        {
            // Mixed errors - Project and DLL or multiple copies of the same DLL

            if (SourceTypeProject->IsMetaData() == TargetTypeProject->IsMetaData())
            {
                // Multiple copies of the same DLL.

                CompilerProject *Project1 =
                    (m_Project && m_Project->IsProjectReferenced(SourceTypeProject)) ?
                        m_Project :
                        SourceTypeProject->GetFirstReferencingProject();

                CompilerProject *Project2 =
                    (m_Project && m_Project->IsProjectReferenced(TargetTypeProject)) ?
                        m_Project :
                        TargetTypeProject->GetFirstReferencingProject();

                if (Project1 && Project2)
                {
                    ReportSemanticError(
                        ERRID_TypeMismatchMixedDLLs6,
                        Input->Loc,
                        SourceType,
                        TargetType,
                        SourceTypeProject->GetFileName(),               // Path to the DLL
                        GetErrorProjectName(Project1),
                        TargetTypeProject->GetFileName(),               // Path to the DLL
                        GetErrorProjectName(Project2));

                    return MakeBad(Input);
                }
            }
            else
            {
                // Project and DLL mixed.

                CompilerProject *MetadataProject =
                    SourceTypeProject->IsMetaData() ? SourceTypeProject : TargetTypeProject;

                CompilerProject *NonMetadataProject =
                    SourceTypeProject->IsMetaData() ? TargetTypeProject : SourceTypeProject;

                CompilerProject *Project1 =
                    (m_Project && m_Project->IsProjectReferenced(MetadataProject)) ?
                        m_Project :
                        MetadataProject->GetFirstReferencingProject();

                if (Project1)
                {
                    ReportSemanticError(
                        ERRID_TypeMismatchDLLProjectMix6,
                        Input->Loc,
                        SourceType,
                        TargetType,
                        GetAssemblyName(MetadataProject),
                        GetErrorProjectName(MetadataProject),
                        GetErrorProjectName(Project1),
                        GetErrorProjectName(NonMetadataProject));

                    return MakeBad(Input);
                }
            }
        }

        // default to normal errors.
        if (XmlLiteralErrorId != 0)
        {
            Type* InterfaceType = NULL;
            InterfaceType = TypeHelpers::IsInterfaceType(TargetType) ? TargetType : SourceType;

            ReportSemanticError(
                XmlLiteralErrorId,
                Input->Loc,
                SourceType,
                TargetType,
                InterfaceType
                );
        }
        else if (TypeHelpers::IsArrayType(SourceType) && TypeHelpers::IsArrayType(TargetType))
        {
            ReportArrayCovarianceMismatch(
                SourceType->PArrayType(),
                TargetType->PArrayType(),
                Input->Loc);
        }
        else
        {
            if (TypeHelpers::IsDateType(SourceType) && TypeHelpers::IsDoubleType(TargetType))
            {
                ReportSemanticError(
                    ERRID_DateToDoubleConversion,
                    Input->Loc,
                    TargetType);
            }
            else if (TypeHelpers::IsDateType(TargetType) && TypeHelpers::IsDoubleType(SourceType))
            {
                ReportSemanticError(
                    ERRID_DoubleToDateConversion,
                    Input->Loc,
                    SourceType);
            }
            else if (TypeHelpers::IsCharType(TargetType) && TypeHelpers::IsIntegralType(SourceType))
            {
                ReportSemanticError(
                    ERRID_IntegralToCharTypeMismatch1,
                    Input->Loc,
                    SourceType);
            }
            else if (TypeHelpers::IsIntegralType(TargetType) && TypeHelpers::IsCharType(SourceType))
            {
                ReportSemanticError(
                    ERRID_CharToIntegralTypeMismatch1,
                    Input->Loc,
                    TargetType);
            }
            else if (CopyBackConversionParam)
            {
                ReportSemanticError(
                    ERRID_CopyBackTypeMismatch3,
                    Input->Loc,
                    CopyBackConversionParam->GetName(),
                    SourceType,
                    TargetType);
            }
            else
            {
                ReportSemanticError(
                    ERRID_TypeMismatch2,
                    Input->Loc,
                    SourceType,
                    TargetType);
            }
        }
    }

    return MakeBad(Input);
}


bool
Semantics::MakeVarianceConversionSuggestion
(
    ILTree::Expression* &Input,
    Type *TargetType,
    ConversionClass ConversionClassification
)
{
    VSASSERT(Input!=NULL && TargetType!=NULL, "unexpected: called MakeVarianceConversionSuggestion with a NULL Input or TargetType");

    // This function is invoked on the occ----ion of a ConversionNarrowing or ConversionError.
    // It looks at the conversion. If the conversion could have been helped by variance in
    // some way, it reports an error/warning message to that effect and returns true. This
    // message is a substitute for whatever other conversion-failed message might have been displayed.
    //
    // Note: these variance-related messages will NOT show auto-correct suggestion of using CType. That's
    // because, in these cases, it's more likely than not that CType will fail, so it would be a bad suggestion

    int ErrorOrWarning;
    if (m_UsingOptionTypeStrict || ConversionClassification == ConversionError )
    {
        ErrorOrWarning = 0; // error
    }
    else if (WarnOptionStrict())
    {
        ErrorOrWarning = 1; // warning
    }
    else
    {
        return false; // nothing
    }

    Type *SourceType = Input->ResultType;

    // Variance scenario 2: Dim x As List(Of Animal) = New List(Of Tiger)
    // "List(Of Tiger) cannot be converted to List(Of Animal). Consider using IEnumerable(Of Animal) instead."
    //
    // (1) If the user attempts a conversion to DEST which is a generic binding of one of the non-variant
    //     standard generic collection types List(Of D), Collection(Of D), ReadOnlyCollection(Of D),
    //     IList(Of D), ICollection(Of D)
    // (2) and if the conversion failed (either ConversionNarrowing or ConversionError),
    // (3) and if the source type SOURCE implemented/inherited exactly one binding ISOURCE=G(Of S) of that
    //     generic collection type G
    // (4) and if there is a reference conversion from S to D
    // (5) Then report "G(Of S) cannot be converted to G(Of D). Consider converting to IEnumerable(Of D) instead."

    FX::FXSymbolProvider *fxs = m_CompilerHost->GetFXSymbolProvider();
    BCSYM *TargetGeneric = TargetType->IsGenericBinding() ? TargetType->PGenericBinding()->GetGeneric() : NULL;

    if (TargetGeneric!=NULL &&
        ((fxs->IsTypeAvailable(FX::GenericListType) && TargetGeneric==fxs->GetType(FX::GenericListType)) ||
         (fxs->IsTypeAvailable(FX::GenericIListType) && TargetGeneric==fxs->GetType(FX::GenericIListType)) ||
         (fxs->IsTypeAvailable(FX::GenericIReadOnlyListType) && TargetGeneric==fxs->GetType(FX::GenericIReadOnlyListType)) ||
         (fxs->IsTypeAvailable(FX::GenericIReadOnlyCollectionType) && TargetGeneric==fxs->GetType(FX::GenericIReadOnlyCollectionType)) ||
         (fxs->IsTypeAvailable(FX::GenericICollectionType) && TargetGeneric==fxs->GetType(FX::GenericICollectionType)) ||
         (fxs->IsTypeAvailable(FX::GenericCollectionType) && TargetGeneric==fxs->GetType(FX::GenericCollectionType)) ||
         (fxs->IsTypeAvailable(FX::GenericReadOnlyCollectionType) && TargetGeneric==fxs->GetType(FX::GenericReadOnlyCollectionType))))
    {
        DynamicArray<GenericTypeBinding *> MatchingGenericBindings;
        TypeHelpers::IsOrInheritsFromOrImplements(
            SourceType,
            TargetType->PGenericTypeBinding()->GetGeneric(),
            m_SymbolCreator,
            false, // don't chase through generic bindings
            &MatchingGenericBindings,
            m_CompilerHost);

        // (3) and if the source type implemented exactly one binding of it...
        if (MatchingGenericBindings.Count() > 0 && TypeHelpers::EquivalentTypeBindings(MatchingGenericBindings))
        {
            GenericTypeBinding *InhSourceType = MatchingGenericBindings.Element(0);
            VPASSERT(BCSYM::AreTypesEqual(InhSourceType->GetGeneric(), TargetType->PGenericTypeBinding()->GetGeneric()), "unexpected: source claimed to inherit from G(...), but now says that it didn't");
            
            BCSYM *SourceArgument = InhSourceType->GetArgument(0);
            BCSYM *DestArgument = TargetType->PGenericBinding()->GetArgument(0);
            ConversionClass ArgumentClassification = ClassifyCLRReferenceConversion(
                                                    DestArgument,
                                                    SourceArgument,
                                                    m_SymbolCreator,
                                                    m_CompilerHost,
                                                    ConversionSemantics::ReferenceConversions,
                                                    0,
                                                    false,NULL,NULL);
            if (ArgumentClassification == ConversionWidening)
            {
                // "'|1' cannot be converted to '|2'. Consider using '|3' instead."
                unsigned int ErrorOrWarningMessages[] = {ERRID_VarianceIEnumerableSuggestion3,WRNID_VarianceIEnumerableSuggestion3};
                unsigned int SubstMessageInto[] = {0, WRNID_ImplicitConversionSubst1};
                // SubstMessageInto: e.g. WRNID_VarianceIEnumerableSuggestion3 is just a string resource
                // that has to be replaced and then substituted into WRNID_ImplicitConversion1, to get the correct BCxxxx code at the end.
                BCSYM *SuggestedArg[1] = {DestArgument};
                BCSYM *Suggestion = this->m_SymbolCreator.GetGenericBinding(false, fxs->GetType(FX::GenericIEnumerableType), SuggestedArg, 1, NULL);
                
                if (SubstMessageInto[ErrorOrWarning] == 0)
                {
                    // If no substitution is required, then we just emit the warning
                    ReportSemanticError(ErrorOrWarningMessages[ErrorOrWarning], Input->Loc, SourceType, TargetType, Suggestion);
                }
                else
                {
                    // But if substitution is requred, then we have to construct the message first...
                    if (m_ReportErrors) // ExtractErrorName isn't protected against being called when !m_ReportErrors (though ReportSemanticError is)
                    {
                        StringBuffer buf, buf1, buf2, buf3;
                        ResLoadStringRepl(ErrorOrWarningMessages[ErrorOrWarning],
                                        &buf,
                                        ExtractErrorName(SourceType, buf1),
                                        ExtractErrorName(TargetType, buf2),
                                        ExtractErrorName(Suggestion, buf3));
                        // and then substitute it into the actual warning/error number
                        ReportSemanticError(SubstMessageInto[ErrorOrWarning], Input->Loc, buf.GetString());
                    }
                }

                if (ErrorOrWarning == 0)
                {
                    Input = MakeBad(Input);
                }
                return true;
            }
        }
    }


    // Variance scenario 1:                                  | Variance scenario 3:
    // dim x as IEnumerable(Of Tiger)=new List(Of Animal)    | Dim x As IFoo(Of Animal) = New MyFoo
    // "List2(Of Animal) cannot be converted to              | "MyFoo cannot be converted to IFoo(Of Animal).
    // IEnumerable2(Of Tiger) because 'Animal' is not derived| Consider changing the 'T' in the definition
    // from 'Tiger', as required for the 'Out' generic       | of interface IFoo(Of T) to an Out type
    // parameter 'T' in 'IEnumerable2(Of Out T)'"            | parameter, Out T."
    //                                                       |
    // (1) If the user attempts a conversion to              | (1) If the user attempts a conversion to some
    //     some target type DEST=G(Of D1,D2,...) which is    |     target type DEST=G(Of D1,D2,...) which is
    //     a generic binding of some variant interface/      |     a generic binding of some interface/delegate
    //     delegate type G(Of T1,T2,...),                    |     type G(...), which NEED NOT be variant!
    // (2) and if the conversion failed (Narrowing/Error),   | (2) and if the type G were defined in source-code,
    // (3) and if the source type SOURCE implemented/        |     not imported metadata. And the converion failed.
    //     inherited exactly one binding INHSOURCE=          | (3) And INHSOURCE=exactly one binding of G
    //     G(Of S1,S2,...) of that generic type G,           | (4) And if ever difference is either Di/Si/Ti
    // (4) and if the only differences between (D1,D2,...)   |     where Ti has In/Out variance, or is
    //     and (S1,S2,...) occur in positions "Di/Si"        |     Dj/Sj/Tj such that Tj has no variance and
    //     such that the corresponding Ti had either In      |     Dj has a CLR conversion to Sj or vice versa
    //     or Out variance                                   | (5) Then pick the first difference Dj/Sj
    // (5) Then pick on the one such difference Si/Di/Ti     | (6) and report "SOURCE cannot be converted to
    // (6) and report "SOURCE cannot be converted to DEST    |     DEST. Consider changing Tj in the definition
    //     because Si is not derived from Di, as required    |     of interface/delegate IFoo(Of T) to an
    //     for the 'In/Out' generic parameter 'T' in         |     In/Out type parameter, In/Out T".
    //     'IEnumerable2(Of Out T)'"                         |

    // (1) If the user attempts a conversion 
    if (TargetType->IsGenericTypeBinding())
    {
        DynamicArray<GenericTypeBinding *> MatchingGenericBindings;
        TypeHelpers::IsOrInheritsFromOrImplements(
            SourceType,
            TargetType->PGenericTypeBinding()->GetGeneric(),
            m_SymbolCreator,
            false, // don't chase through generic bindings
            &MatchingGenericBindings,
            m_CompilerHost);

        // (3) and if the source type implemented exactly one binding of it...
        if (MatchingGenericBindings.Count() > 0 && TypeHelpers::EquivalentTypeBindings(MatchingGenericBindings))
        {
            GenericTypeBinding *InhSourceType = MatchingGenericBindings.Element(0);
            VPASSERT(BCSYM::AreTypesEqual(InhSourceType->GetGeneric(), TargetType->PGenericTypeBinding()->GetGeneric()), "unexpected: source claimed to inherit from G(...), but now says that it didn't");

            // Fetch all the Si/Di/Ti...
            DynamicArray<VarianceParameterCompatibility> ParameterDetails;
            ClassifyImmediateVarianceCompatibility( TargetType,
                                                    InhSourceType,
                                                    m_SymbolCreator,
                                                    m_CompilerHost,
                                                    ConversionSemantics::ReferenceConversions,
                                                    0,
                                                    NULL,NULL,NULL,
                                                    &ParameterDetails);

            // (4) to test whether the only differences were in positions Di/Si/Ti or Dj/Sj/Tj...
            VarianceParameterCompatibility *OneVariantDifference = NULL;  // for Di/Si/Ti
            VarianceParameterCompatibility *OneInvariantConvertibleDifference = NULL; // for Dj/Sj/Tj where Sj<Dj
            VarianceParameterCompatibility *OneInvariantReverseConvertibleDifference = NULL;  // Dj/Sj/Tj where Dj<Sj
            VarianceParameterCompatibility *OneInvariantIncomensurableDifference = NULL;  // Dk/Sk/Tk where no conversion
            //
            for (ULONG i=0; i<ParameterDetails.Count(); i++)
            {
                if (ParameterDetails.Element(i).Compatible)
                {
                    continue;
                }
                else if (ParameterDetails.Element(i).Param->GetVariance() != Variance_None)
                {
                    OneVariantDifference = &ParameterDetails.Element(i);
                }
                else if (ConversionWidening == ClassifyPredefinedCLRConversion(
                                                        ParameterDetails.Element(i).TargetArgument,
                                                        ParameterDetails.Element(i).SourceArgument,
                                                        ConversionSemantics::ReferenceConversions))
                {
                    OneInvariantConvertibleDifference = &ParameterDetails.Element(i);
                }
                else if (ConversionWidening == ClassifyPredefinedCLRConversion(
                                                        ParameterDetails.Element(i).SourceArgument,
                                                        ParameterDetails.Element(i).TargetArgument,
                                                        ConversionSemantics::ReferenceConversions))
                {
                    OneInvariantReverseConvertibleDifference = &ParameterDetails.Element(i);
                }
                else
                {
                    OneInvariantIncomensurableDifference = &ParameterDetails.Element(i);
                }
            }

            // (5) If a Di/Si/Ti, and no Dj/Sj/Tj nor Dk/Sk/Tk, then report...
            if (OneVariantDifference!=NULL && OneInvariantConvertibleDifference==NULL &&
                OneInvariantReverseConvertibleDifference==NULL && OneInvariantIncomensurableDifference==NULL)
            {
                VSASSERT(OneVariantDifference->Param->GetVariance()==Variance_In || OneVariantDifference->Param->GetVariance()==Variance_Out, "internal logic error: should only have got here if we had a variance disagreement");
                unsigned int ErrorOrWarningMessages[2];
                unsigned int SubstMessageInto[2]; // see comment above about SubstMessageInto
                BCSYM *DerivedArgument, *BaseArgument;
                //
                if (OneVariantDifference->Param->GetVariance() == Variance_Out)
                {
                    ErrorOrWarningMessages[0] = ERRID_VarianceConversionFailedOut6;
                    ErrorOrWarningMessages[1] = WRNID_VarianceConversionFailedOut6;
                    SubstMessageInto[0] = 0;
                    SubstMessageInto[1] = WRNID_ImplicitConversionSubst1;
                    DerivedArgument = OneVariantDifference->SourceArgument;
                    BaseArgument = OneVariantDifference->TargetArgument;
                }
                else
                {
                    ErrorOrWarningMessages[0] = ERRID_VarianceConversionFailedIn6;
                    ErrorOrWarningMessages[1] = WRNID_VarianceConversionFailedIn6;
                    SubstMessageInto[0] = 0;
                    SubstMessageInto[1] = WRNID_ImplicitConversionSubst1;
                    DerivedArgument = OneVariantDifference->TargetArgument;
                    BaseArgument = OneVariantDifference->SourceArgument;
                }

                // "['|5' cannot be converted to '|6' because] '|1' is not derived from '|2',
                // as required for the 'In/Out' generic parameter '|3' in '|4'."
                StringBuffer buf1,buf2,buf3,buf4,buf5,buf6;
                OneVariantDifference->Generic->GetBasicRep(m_Compiler, NULL, &buf4);
                if (m_ReportErrors && m_Errors!=NULL) // Dev10#519024
                {
                    if (SubstMessageInto[ErrorOrWarning]==0)
                    {
                        // If no substitution is required, we can just emit the warning
                        ReportSemanticError(ErrorOrWarningMessages[ErrorOrWarning],
                                            Input->Loc,
                                            DerivedArgument,
                                            BaseArgument,
                                            ExtractErrorName(OneVariantDifference->Param, buf3),
                                            buf4.GetString(),
                                            ExtractErrorName(SourceType, buf5),
                                            ExtractErrorName(TargetType, buf6));
                    }
                    else
                    {
                        // But if substitution is requred, then we have to construct the message first...
                        if (m_ReportErrors) // ExtractErrorName isn't protected against being called when !m_ReportErrors
                        {
                            StringBuffer buf;
                            ResLoadStringRepl(ErrorOrWarningMessages[ErrorOrWarning],
                                            &buf,
                                            ExtractErrorName(DerivedArgument, buf1),
                                            ExtractErrorName(BaseArgument, buf2),
                                            ExtractErrorName(OneVariantDifference->Param, buf3), 
                                            buf4.GetString(),
                                            ExtractErrorName(SourceType, buf5),
                                            ExtractErrorName(TargetType, buf6));
                            // and then substitute it into the actual warning/error number
                            ReportSemanticError(SubstMessageInto[ErrorOrWarning], Input->Loc, buf.GetString());
                        }
                    }
                }
                if (ErrorOrWarning == 0)
                {
                    Input = MakeBad(Input);
                }
                return true;
            }

            // (5b) Otherwise, if a Dj/Sj/Tj and no Dk/Sk/Tk, and G came not from metadata, then report...
            if (OneInvariantIncomensurableDifference==NULL &&
                (OneInvariantConvertibleDifference!=NULL || OneInvariantReverseConvertibleDifference!=NULL) &&
                !TargetType->PGenericBinding()->GetGeneric()->GetCompilerFile()->IsMetaDataFile())
            {
                VarianceParameterCompatibility *OneInvariantDifference;
                unsigned int ErrorOrWarningMessages[2];
                unsigned int SubstMessageInto[2]; // see comment above about SubstMessageInto
                //
                if (OneInvariantConvertibleDifference!=NULL)
                {
                    ErrorOrWarningMessages[0] = ERRID_VarianceConversionFailedTryOut4;
                    ErrorOrWarningMessages[1] = WRNID_VarianceConversionFailedTryOut4;
                    SubstMessageInto[0] = 0;
                    SubstMessageInto[1] = WRNID_ImplicitConversionSubst1;
                    OneInvariantDifference = OneInvariantConvertibleDifference;
                }
                else
                {
                    ErrorOrWarningMessages[0] = ERRID_VarianceConversionFailedTryIn4;
                    ErrorOrWarningMessages[1] = WRNID_VarianceConversionFailedTryIn4;
                    SubstMessageInto[0] = 0;
                    SubstMessageInto[1] = WRNID_ImplicitConversionSubst1;
                    OneInvariantDifference = OneInvariantReverseConvertibleDifference;
                }

                // "'|1' cannot be converted to '|2'. Consider changing the '|3' in the definition of '|4' to an Out/In type parameter, 'Out/In |3'."
                StringBuffer buf1, buf2, buf3, buf4;
                OneInvariantDifference->Generic->GetBasicRep(m_Compiler, NULL, &buf4);
                if (m_ReportErrors && m_Errors!=NULL) // Dev10#519024
                {
                    if (SubstMessageInto[ErrorOrWarning]==0)
                    {
                        // If no substitution is required, we can just emit the warning
                        ReportSemanticError(ErrorOrWarningMessages[ErrorOrWarning],
                                            Input->Loc,
                                            SourceType,
                                            TargetType,
                                            ExtractErrorName(OneInvariantDifference->Param, buf3),
                                            buf4.GetString());
                    }
                    else
                    {
                        // But if substitution is requred, then we have to construct the message first...
                        if (m_ReportErrors) // ExtractErrorName isn't protected against being called when !m_ReportErrors
                        {
                            StringBuffer buf;
                            ResLoadStringRepl(ErrorOrWarningMessages[ErrorOrWarning],
                                            &buf,
                                            ExtractErrorName(SourceType, buf1),
                                            ExtractErrorName(TargetType, buf2),
                                            ExtractErrorName(OneInvariantDifference->Param, buf3), 
                                            buf4.GetString());
                            // and then substitute it into the actual warning/error number
                            ReportSemanticError(SubstMessageInto[ErrorOrWarning], Input->Loc, buf.GetString());
                        }
                    }
                }
                if (ErrorOrWarning == 0)
                {
                    Input = MakeBad(Input);
                }
                return true;
            }
        }
    }

    return false;
}


bool
Semantics::IsValidAttributeArrayConstant
(
    ILTree::Expression *PossibleConstant
)
{
   // 1-D array parameters can show up in attributes
    //
    if (TypeHelpers::IsArrayType(PossibleConstant->ResultType) &&
         PossibleConstant->ResultType->PArrayType()->GetRank() == 1 &&
         IsValidAttributeType(TypeHelpers::GetElementType(PossibleConstant->ResultType->PArrayType()), m_CompilerHost) &&
         !TypeHelpers::IsArrayType(TypeHelpers::GetElementType(PossibleConstant->ResultType->PArrayType())))
    {
        if (PossibleConstant->bilop != SX_CTYPE &&
             PossibleConstant->bilop != SX_WIDE_COERCE)
        {
            return true;
        }

        if (PossibleConstant->AsBinaryExpression().Left->ResultType->IsArrayType())
        {
            Type *SourceType = TypeHelpers::GetElementType(PossibleConstant->AsBinaryExpression().Left->ResultType->PArrayType());
            Type *TargetType = TypeHelpers::GetElementType(PossibleConstant->ResultType->PArrayType());

            if (TypeHelpers::EquivalentTypes(SourceType, TargetType) ||
                (TypeHelpers::IsIntegralType(SourceType) && SourceType->GetVtype() == TargetType->GetVtype()))
            {
                return true;
            }
        }
    }

    return false;
}

bool
Semantics::IsValidAttributeConstant
(
    ILTree::Expression *PossibleConstant
)
{
    if (IsConstant(PossibleConstant))
    {
        return true;
    }

    // GetType(Foo) is allowed in attribute arguments
    //
    if (GetFXSymbolProvider()->IsTypeAvailable(FX::TypeType) &&
        TypeHelpers::EquivalentTypes(PossibleConstant->ResultType, GetFXSymbolProvider()->GetTypeType()))
    {
        return true;
    }

    // Object parameters can show up in attributes, so when passing
    // constant values to them, a cast to object is involved
    //
    if ((PossibleConstant->bilop == SX_CTYPE ||
            PossibleConstant->bilop == SX_WIDE_COERCE) &&
        PossibleConstant->ResultType->IsObject() &&
        IsValidAttributeType(PossibleConstant->AsBinaryExpression().Left->ResultType, m_CompilerHost))
    {
        return true;
    }

    // Allow constant 1-d arrays.
    return IsValidAttributeArrayConstant(PossibleConstant);
}

void
Semantics::ReportArrayCovarianceMismatch
(
    ArrayType *SourceArray,
    ArrayType *TargetArray,
    const Location &ErrorLocation
)
{
    // The element types must either be the same or (Byref parameter conformance
    // excepted) or the source element type must extend or implement the
    // target element type. (VB implements array covariance.)

    Type *SourceElementType = TypeHelpers::GetElementType(SourceArray);
    Type *TargetElementType = TypeHelpers::GetElementType(TargetArray);

    if (TypeHelpers::IsBadType(SourceElementType) || TypeHelpers::IsBadType(TargetElementType))
    {
        if (TypeHelpers::IsBadType(SourceElementType))
        {
            ReportBadType(SourceElementType, ErrorLocation);
        }

        if (TypeHelpers::IsBadType(TargetElementType))
        {
            ReportBadType(TargetElementType, ErrorLocation);
        }

        return;
    }

    if (!TypeHelpers::EquivalentTypes(SourceElementType, TargetElementType) &&
        ((!TypeHelpers::IsClassType(SourceElementType) ||
          !TypeHelpers::IsClassOrInterfaceType(TargetElementType) ||
          TypeHelpers::IsRootObjectType(SourceElementType) ||
          !IsOrInheritsFromOrImplements(SourceElementType, TargetElementType)) &&
         (!TypeHelpers::IsRootObjectType(TargetElementType) ||
          !(TypeHelpers::IsStringType(SourceElementType) || TypeHelpers::IsArrayType(SourceElementType)))))
    {
        if (TypeHelpers::IsRootObjectType(TargetElementType) ||
            TypeHelpers::IsRootValueType(TargetElementType, m_CompilerHost))
        {
            ReportSemanticError(
                ERRID_ConvertObjectArrayMismatch3,
                ErrorLocation,
                SourceArray,
                TargetArray,
                SourceElementType);
        }
        else
        {
            ReportSemanticError(
                ERRID_ConvertArrayMismatch4,
                ErrorLocation,
                SourceArray,
                TargetArray,
                SourceElementType,
                TargetElementType);
        }

        return;
    }

    // The ranks must agree.

    if (TargetArray->GetRank() != SourceArray->GetRank())
    {
        ReportSemanticError(
            ERRID_ConvertArrayRankMismatch2,
            ErrorLocation,
            SourceArray,
            TargetArray);

        return;
    }

    ReportSemanticError(
        ERRID_TypeMismatch2,
        ErrorLocation,
        SourceArray,
        TargetArray);

    return;
}

bool
Semantics::CanAccessDefaultPropertyThroughType
(
    Type *TypeToCheck,
    CompilerHost *TheCompilerHost
)
{
    return (TypeHelpers::IsClassOrInterfaceType(TypeToCheck) &&
            !TypeHelpers::IsRootObjectType(TypeToCheck) &&
            TypeToCheck != TheCompilerHost->GetFXSymbolProvider()->GetType(FX::ArrayType) &&
            // Delegates have an Invoke method. In order to get to it in an expression
            // of the form "Del(Args)", don't access the default property.
            !TypeHelpers::IsDelegateType(TypeToCheck)) ||
           TypeHelpers::IsRecordType(TypeToCheck) ||
           TypeHelpers::IsGenericParameter(TypeToCheck);
}

ILTree::Expression *
Semantics::AccessDefaultProperty
(
    const Location &TextSpan,
    ILTree::Expression *Input,
    typeChars TypeCharacter,
    ExpressionFlags Flags
)
{
    if (CanAccessDefaultPropertyThroughType(Input->ResultType, m_CompilerHost))
    {
        bool PropertyIsBad = false;
        Declaration *DefaultProperty = NULL;
        GenericBinding *DefaultPropertyGenericBindingContext = NULL;

        VSASSERT(
            TypeHelpers::IsClassOrRecordType(Input->ResultType) ||
            TypeHelpers::IsInterfaceType(Input->ResultType) ||
            TypeHelpers::IsGenericParameter(Input->ResultType), "Expected type isn't.");

        DefaultProperty =
            LookupDefaultProperty(
                Input->ResultType,
                Input->Loc,
                PropertyIsBad,
                &DefaultPropertyGenericBindingContext);

        if (PropertyIsBad)
        {
            return MakeBad(Input);
        }

        if (DefaultProperty)
        {
            return
                ReferToSymbol(
                    TextSpan,
                    DefaultProperty,
                    TypeCharacter,
                    Input,
                    DefaultPropertyGenericBindingContext,
                    Flags);
        }
        else
        {
            // Define a "fake" indexer on all queryable sources, which maps to an ElementAtOrDefault method on the source
            Type * ControlVariableType = NULL;
            QueryExpressionFlags QueryFlags = QueryNoFlags;
            ILTree::Expression * QueryableSource = ToQueryableSource(Input, &ControlVariableType, &QueryFlags);

            if (QueryableSource && !IsBad(QueryableSource) && ControlVariableType)
            {
                ParserHelper PH(&m_TreeStorage, TextSpan);

                return
                    InterpretExpression(
                        PH.CreateQualifiedExpression(
                            PH.CreateBoundExpression(QueryableSource),
                            PH.CreateNameExpression(STRING_CONST(m_Compiler, ElementAtMethod), Location::GetHiddenLocation())),
                        Flags);
            }

            ReportSemanticError(
                TypeHelpers::IsClassType(Input->ResultType) ?
                    ERRID_NoDefaultNotExtend1 :
                    (TypeHelpers::IsRecordType(Input->ResultType) ?
                        ERRID_StructureNoDefault1 :
                        ERRID_InterfaceNoDefault1),
                Input->Loc,
                Input->ResultType);

            return MakeBad(Input);
        }
    }

    return Input;
}

ILTree::Expression *
Semantics::ProduceConstantExpression
(
    ConstantValue Value,
    const Location &ExpressionLocation,
    Type *ResultType
    IDE_ARG(unsigned Flags)
)
{
    if (ResultType == nullptr)
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    if (Value.TypeCode == t_ref || Value.TypeCode == t_array)
    {
        // Whenever we see t_ref or t_array as the type of a constant value, well, that constant value must
        // be Nothing.
        VSASSERT(Value.Integral == 0, "Unexpected: non-Nothing constant value of type t_ref/t_array");
        // For records, if given "Nothing" then we have to emit a "Nothing" node of type Object
        // (which will later be converted to the appropriate type). Note: as the following assert shows,
        // all nullables are also records, so the condition below (IsRecordType) also catches nullables.
        VSASSERT(TypeHelpers::IsRecordType(ResultType) || !TypeHelpers::IsNullableType(ResultType), "unexpected: a nullable which isn't a record");
        // OPTIONAL:
        return AllocateExpression(SX_NOTHING, TypeHelpers::IsRecordType(ResultType) ? GetFXSymbolProvider()->GetObjectType() : ResultType, ExpressionLocation);
    }

    if (TypeHelpers::IsRootObjectType(ResultType))
    {
        ResultType = m_CompilerHost->GetFXSymbolProvider()->GetType(Value.TypeCode);
    }

    if (ResultType->GetVtype() != Value.TypeCode)
    {
        VSFAIL("unexpected mismatch between type of constant and requested result type. : #10/3/2003#");
        ReportSemanticError(
            ERRID_InternalCompilerError,
            ExpressionLocation);
        return AllocateBadExpression(ExpressionLocation);
    }

    switch (Value.TypeCode)
    {
        case t_i1:
        case t_ui1:
        case t_i2:
        case t_ui2:
        case t_i4:
        case t_ui4:
        case t_i8:
        case t_ui8:
        case t_bool:
        case t_char:
        case t_date:
        {
            return ProduceConstantExpression(Value.Integral, ExpressionLocation, ResultType IDE_ARG(Flags));
        }

        case t_single:
            return ProduceFloatingConstantExpression((double)Value.Single, ExpressionLocation, ResultType IDE_ARG(Flags));

        case t_double:
            return ProduceFloatingConstantExpression(Value.Double, ExpressionLocation, ResultType IDE_ARG(Flags));

        case t_decimal:
            return ProduceDecimalConstantExpression(Value.Decimal, ExpressionLocation IDE_ARG(Flags));

        case t_string:
        {
            ILTree::Expression *Result;

            // A string constant always has a spelling, unless it is Nothing.
            if (Value.String.Spelling)
            {
                Result =
                    ProduceStringConstantExpression(
                        Value.String.Spelling,
                        Value.String.LengthInCharacters,
                        ExpressionLocation
                        IDE_ARG(Flags));
            }
            else
            {
                Result = AllocateExpression(SX_NOTHING, GetFXSymbolProvider()->GetStringType(), ExpressionLocation);
            }

            return Result;
        }

        case t_bad:

            // This occurs only if something has already been reported for something
            // related to the expression.

            return AllocateBadExpression(ExpressionLocation);

        default:
            VSFAIL("ConstantValue does not contain a valid compile-time value.");
            ReportSemanticError(
                ERRID_InternalCompilerError,
                ExpressionLocation);
            return AllocateBadExpression(ExpressionLocation);
    }
}

ILTree::Expression *
Semantics::ProduceConstantExpression
(
    Quadword Value,
    const Location &ExpressionLocation,
    Type *ResultType
    IDE_ARG(unsigned Flags)
)
{
    if (ResultType == nullptr)
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    VSASSERT(TypeHelpers::IsIntegralType(ResultType) || TypeHelpers::IsCharType(ResultType) || TypeHelpers::IsBooleanType(ResultType) || TypeHelpers::IsDateType(ResultType),
             "Non-integral type pun.");

    ILTree::Expression *Result = AllocateExpression(SX_CNS_INT, ResultType, ExpressionLocation);
    Result->AsIntegralConstantExpression().Value = Value;
    IDE_CODE(Result->AsIntegralConstantExpression().uFlags |= Flags);
    return Result;
}

ILTree::Expression *
Semantics::ProduceFloatingConstantExpression
(
    double Value,
    const Location &ExpressionLocation,
    Type *ResultType
    IDE_ARG(unsigned Flags)
)
{
    if (ResultType == nullptr)
    {
        return AllocateBadExpression(ExpressionLocation);
    }

    VSASSERT(TypeHelpers::IsFloatingType(ResultType), "Non-floating type pun.");

    ILTree::Expression *Result = AllocateExpression(SX_CNS_FLT, ResultType, ExpressionLocation);
    Result->AsFloatConstantExpression().Value = Value;
    IDE_CODE(Result->AsFloatConstantExpression().uFlags |= Flags);
    return Result;
}

ILTree::Expression *
Semantics::ProduceStringConstantExpression
(
    _In_opt_count_(LengthInCharacters) const WCHAR *Spelling,
    size_t LengthInCharacters,
    const Location &ExpressionLocation
    IDE_ARG(unsigned Flags)
)
{
    ILTree::Expression *Result = AllocateExpression(SX_CNS_STR, GetFXSymbolProvider()->GetStringType(), ExpressionLocation);
    Result->AsStringConstant().Spelling = Spelling;
    Result->AsStringConstant().Length = LengthInCharacters;
    IDE_CODE(Result->AsStringConstant().uFlags |= Flags);
    return Result;
}

ILTree::Expression *
Semantics::ProduceDecimalConstantExpression
(
    DECIMAL Value,
    const Location &ExpressionLocation
    IDE_ARG(unsigned Flags)
)
{
    ILTree::Expression *Result = AllocateExpression(SX_CNS_DEC, GetFXSymbolProvider()->GetDecimalType(), ExpressionLocation);
    Result->AsDecimalConstantExpression().Value = Value;
    IDE_CODE(Result->AsDecimalConstantExpression().uFlags |= Flags);
    return Result;
}

ConstantValue
Semantics::ExtractConstantValue
(
    ILTree::Expression *Input
)
{
    VSASSERT(IsConstant(Input), "Advertised constant isn't.");

    ConstantValue Result;

    Result.TypeCode = Input->ResultType->GetVtype();

    switch (Result.TypeCode)
    {
        case t_bool:
        case t_i1:
        case t_ui1:
        case t_i2:
        case t_ui2:
        case t_i4:
        case t_ui4:
        case t_i8:
        case t_ui8:
        case t_date:
        case t_char:

            Result.Integral = Input->AsIntegralConstantExpression().Value;
            break;

        case t_single:

            Result.Single = (float)Input->AsFloatConstantExpression().Value;
            break;

        case t_double:

            Result.Double = Input->AsFloatConstantExpression().Value;
            break;

        case t_string:

            if (Input->bilop != SX_NOTHING)
            {
                Result.String.Spelling = Input->AsStringConstant().Spelling;
                Result.String.LengthInCharacters = (unsigned)Input->AsStringConstant().Length;
            }
            break;

        case t_decimal:

            Result.Decimal = Input->AsDecimalConstantExpression().Value;
            break;

        case t_ref:
        case t_array:
            VSASSERT(
                Input->bilop == SX_NOTHING,
                "Invalid constant value for class or array.");
            break;

        default:

            VSFAIL("Unexpected type in extracting constant value.");
            break;
    }

    return Result;
}

Quadword
NarrowIntegralResult
(
    Quadword SourceValue,
    Vtypes vtSourceType,
    Vtypes vtResultType,
    bool &Overflow
)
{
    Quadword ResultValue = 0;

    switch (vtResultType)
    {
        case t_bool:
            ResultValue = (SourceValue == 0 ? COMPLUS_FALSE : COMPLUS_TRUE);
            return ResultValue;
        case t_i1:
            ResultValue = (__int8)SourceValue;
            break;
        case t_ui1:
            ResultValue = (unsigned __int8)SourceValue;
            break;
        case t_i2:
            ResultValue = (__int16)SourceValue;
            break;
        case t_ui2:
            ResultValue = (unsigned __int16)SourceValue;
            break;
        case t_i4:
            ResultValue = (__int32)SourceValue;
            break;
        case t_ui4:
            ResultValue = (unsigned __int32)SourceValue;
            break;
        case t_i8:
            ResultValue = (__int64)SourceValue;
            break;
        case t_ui8:
            ResultValue = (unsigned __int64)SourceValue;
            break;
        case t_char:
            ResultValue = (unsigned __int16)SourceValue;
            // ?? overflow?
            break;
        default:
            VSFAIL("Surprising target integral type.");
    }

    if (!IsBooleanType(vtSourceType) && (IsUnsignedType(vtSourceType) ^ IsUnsignedType(vtResultType)))
    {
        // If source is a signed type and is a negative value or
        // target is a signed type and results in a negative
        // value, indicate overflow.

        if (!IsUnsignedType(vtSourceType))
        {
            if (SourceValue >> (sizeof(SourceValue) * 8 - 1))
            {
                Overflow = true;
            }
        }
        else
        {
            VSASSERT(!IsUnsignedType(vtResultType), "Expected signed Target type!!!");

            if (ResultValue >> (sizeof(ResultValue) * 8 - 1))
            {
                Overflow = true;
            }
        }
    }

    if (ResultValue != SourceValue)
    {
        Overflow = true;
    }

    return ResultValue;

}


// Narrow a quadword result to a specific integral type, setting Overflow true
// if the result value cannot be represented in the result type.
Quadword
NarrowIntegralResult
(
    Quadword SourceValue,
    Type *SourceType,
    Type *ResultType,
    bool &Overflow
)
{
    if (SourceType == nullptr || ResultType == nullptr)
    {
        return SourceValue;
    }

    VSASSERT( TypeHelpers::IsIntegralType(SourceType) || TypeHelpers::IsBooleanType(SourceType) || TypeHelpers::IsCharType(SourceType),
                    "Unexpected source type passed in to conversion function!!!");

    return NarrowIntegralResult(SourceValue, SourceType->GetVtype(), ResultType->GetVtype(), Overflow);
}

// ForceNarrowingToSingle must not be inlined. See the "ISSUE" comment below.

static float
ForceNarrowingToSingle
(
    float Value
)
{
    return Value;
}

double
NarrowFloatingResult
(
    double Result,
    Vtypes vtResultType,
    bool &Overflow
)
{
    if (IsInvalidDoubleValue(Result))
    {
        Overflow = true;
    }

    switch (vtResultType)
    {
        case t_double:
            return Result;
        case t_single:
        {
            if (Result > MAX_SINGLE ||
                Result < MIN_SINGLE)
            {
                Overflow = true;
            }
            // ISSUE: There appears to be no way to force the C++ compiler to respect
            // a cast to float, including (if compiling with optimization enabled)
            // introducing volatile storage and using pointers.
            //
            // If ForceNarrowingToSingle gets inlined, the narrowing will therefore
            // not occur. Turning off optimization by pragma apparently does not
            // work reliably. Introducing a call via a method pointer apparently works
            // to defeat the inliner.

            typedef float Narrower(float);
            Narrower *Narrow = &ForceNarrowingToSingle;
            return Narrow((float)Result);
        }

        default:
            VSFAIL("Surprising floating type.");
    }

    return Result;
}

// Narrow a double result to a specific floating type, setting Overflow true
// if the result value cannot be represented in the result type.
double
NarrowFloatingResult
(
    double Result,
    Type *ResultType,
    bool &Overflow
)
{
    if (ResultType != nullptr)
    {
        return NarrowFloatingResult(Result, ResultType->GetVtype(), Overflow);
    }

    return Result;
}

static Quadword
Multiply
(
    Quadword LeftValue,
    Quadword RightValue,
    Type *SourceType,
    Type *ResultType,
    bool &Overflow
)
{
    Quadword ResultValue =
        NarrowIntegralResult(
            LeftValue * RightValue,
            SourceType,
            ResultType,
            Overflow);

    if (TypeHelpers::IsUnsignedType(ResultType))
    {
        if (RightValue != 0 &&
                (unsigned __int64)ResultValue / (unsigned __int64)RightValue != (unsigned __int64)LeftValue)
        {
            Overflow = true;
        }
    }
    else
    {
        if ((LeftValue > 0 && RightValue > 0 && ResultValue <= 0) ||
            (LeftValue < 0 && RightValue < 0 && ResultValue <= 0) ||
            (LeftValue > 0 && RightValue < 0 && ResultValue >= 0) ||
            (LeftValue < 0 && RightValue > 0 && ResultValue >= 0) ||
            (RightValue != 0 && ResultValue / RightValue != LeftValue))
        {
            Overflow = true;
        }
    }

    return ResultValue;
}

bool
IsInvalidDoubleValue
(
    double Value
)
{
    int BitsToCheck = (*(((int *)&Value) + 1)) & 0xfff00000;

    return BitsToCheck == 0xfff00000 || BitsToCheck == 0x7ff00000;
}

#define CLR_NAN_64 UINT64(0xFFF8000000000000)
#define IS_DBL_INFINITY(x) ((*((UINT64 *)((void *)&x)) & UINT64(0x7FFFFFFFFFFFFFFF)) == UINT64(0x7FF0000000000000))
#define IS_DBL_ONE(x)      ((*((UINT64 *)((void *)&x))) == UINT64(0x3FF0000000000000))
#define IS_DBL_NEGATIVEONE(x)      ((*((UINT64 *)((void *)&x))) == UINT64(0xBFF0000000000000))

ILTree::Expression *
Semantics::PerformCompileTimeBinaryOperation
(
    BILOP Opcode,
    Type *ResultType,
    const Location &ExpressionLocation,
    ILTree::Expression *Left,
    ILTree::Expression *Right
)
{
    VSASSERT(AllowsCompileTimeOperations(ResultType), "Unwelcome type for compile-time expression evaluation.");

    VSASSERT(IsConstant(Left), "Expected a constant.");
    VSASSERT(IsConstant(Right), "Expected a constant.");

    VSASSERT(IsShiftOperator(Opcode) || TypeHelpers::EquivalentTypes(Left->ResultType, Right->ResultType), "Binary operation on mismatched types.");

    if (TypeHelpers::IsIntegralType(Left->ResultType) || TypeHelpers::IsCharType(Left->ResultType) || TypeHelpers::IsDateType(Left->ResultType))
    {
        Quadword LeftValue = Left->AsIntegralConstantExpression().Value;
        Quadword RightValue = Right->AsIntegralConstantExpression().Value;

        if (TypeHelpers::IsBooleanType(ResultType))
        {
            bool ComparisonSucceeds = false;

            switch (Opcode)
            {
                case SX_EQ:
                    ComparisonSucceeds =
                        TypeHelpers::IsUnsignedType(Left->ResultType) ?
                            (unsigned __int64)LeftValue == (unsigned __int64)RightValue :
                            LeftValue == RightValue;
                    break;
                case SX_NE:
                    ComparisonSucceeds =
                        TypeHelpers::IsUnsignedType(Left->ResultType) ?
                            (unsigned __int64)LeftValue != (unsigned __int64)RightValue :
                            LeftValue != RightValue;
                    break;
                case SX_LE:
                    ComparisonSucceeds =
                        TypeHelpers::IsUnsignedType(Left->ResultType) ?
                            (unsigned __int64)LeftValue <= (unsigned __int64)RightValue :
                            LeftValue <= RightValue;
                    break;
                case SX_GE:
                    ComparisonSucceeds =
                        TypeHelpers::IsUnsignedType(Left->ResultType) ?
                            (unsigned __int64)LeftValue >= (unsigned __int64)RightValue :
                            LeftValue >= RightValue;
                    break;
                case SX_LT:
                    ComparisonSucceeds =
                        TypeHelpers::IsUnsignedType(Left->ResultType) ?
                            (unsigned __int64)LeftValue < (unsigned __int64)RightValue :
                            LeftValue < RightValue;
                    break;
                case SX_GT:
                    ComparisonSucceeds =
                        TypeHelpers::IsUnsignedType(Left->ResultType) ?
                            (unsigned __int64)LeftValue > (unsigned __int64)RightValue :
                            LeftValue > RightValue;
                    break;

                default:
                    VSFAIL("Surprising boolean operation.");
            }

            return
                ProduceConstantExpression(
                    ComparisonSucceeds ? COMPLUS_TRUE : COMPLUS_FALSE,
                    ExpressionLocation,
                    GetFXSymbolProvider()->GetBooleanType()
                    IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
        }

        else
        {
            // Compute the result in 64-bit arithmetic, and determine if the
            // operation overflows the result type.

            Quadword ResultValue = 0;
            bool Overflow = false;

            switch (Opcode)
            {
                case SX_ADD:
                    ResultValue =
                        NarrowIntegralResult(
                            LeftValue + RightValue,
                            Left->ResultType,
                            ResultType,
                            Overflow);
                    if (!TypeHelpers::IsUnsignedType(ResultType))
                    {
                        if ((RightValue > 0 && ResultValue < LeftValue) ||
                            (RightValue < 0 && ResultValue > LeftValue))
                        {
                            Overflow = true;
                        }
                    }
                    else if ((unsigned __int64)ResultValue < (unsigned __int64)LeftValue)
                    {
                        Overflow = true;
                    }
                    break;

                case SX_SUB:
                    ResultValue =
                        NarrowIntegralResult(
                            LeftValue - RightValue,
                            Left->ResultType,
                            ResultType,
                            Overflow);
                    if (!TypeHelpers::IsUnsignedType(ResultType))
                    {
                        if ((RightValue > 0 && ResultValue > LeftValue) ||
                            (RightValue < 0 && ResultValue < LeftValue))
                        {
                            Overflow = true;
                        }
                    }
                    else if ((unsigned __int64)ResultValue > (unsigned __int64)LeftValue)
                    {
                        Overflow = true;
                    }
                    break;

                case SX_MUL:
                    ResultValue = Multiply(LeftValue, RightValue, Left->ResultType, ResultType, Overflow);
                    break;

                case SX_IDIV:
                    if (RightValue == 0)
                    {
                        ReportSemanticError(
                            ERRID_ZeroDivide,
                            ExpressionLocation);

                        return AllocateBadExpression(ExpressionLocation);
                    }

                        ResultValue =
                            NarrowIntegralResult(
                                TypeHelpers::IsUnsignedType(ResultType) ?
                                    (unsigned __int64) LeftValue / (unsigned __int64) RightValue :
                                    LeftValue / RightValue,
                                Left->ResultType,
                                ResultType,
                                Overflow);

                    if (!TypeHelpers::IsUnsignedType(ResultType) && LeftValue == _I64_MIN && RightValue == -1)
                    {
                        Overflow = true;
                    }
                    break;

                case SX_MOD:
                    if (RightValue == 0)
                    {
                        ReportSemanticError(
                            ERRID_ZeroDivide,
                            ExpressionLocation);

                        return AllocateBadExpression(ExpressionLocation);
                    }

                    if (TypeHelpers::IsUnsignedType(ResultType))
                    {
                        ResultValue = (unsigned __int64)LeftValue % (unsigned __int64)RightValue;
                    }
                    // 64-bit processors crash on 0, -1 (
                    else if (RightValue != ~(__int64)0)
                    {
                        ResultValue = LeftValue % RightValue;
                    }
                    else
                    {
                        ResultValue = 0;
                    }
                    break;

                case SX_XOR:
                    ResultValue = LeftValue ^ RightValue;
                    break;

                case SX_OR:
                    ResultValue = LeftValue | RightValue;
                    break;

                case SX_AND:
                    ResultValue = LeftValue & RightValue;
                    break;

                case SX_SHIFT_LEFT:
                    {
                        VSASSERT(
                            RightValue >= 0 && RightValue <= GetShiftSizeMask(ResultType->GetVtype()),
                            "Unexpected shift amount.");

                        ResultValue = LeftValue << RightValue;

                        // Round-trip the result through a cast.  We do this for two reasons:
                        // a) Bits may have shifted off the end and need to be stripped away.
                        // b) The sign bit may have changed which requires the result to be sign-extended.

                        bool OverflowTemp = false;
                        ResultValue = NarrowIntegralResult(ResultValue, Left->ResultType, ResultType, OverflowTemp);
                    }
                    break;

                case SX_SHIFT_RIGHT:
                    VSASSERT(
                        RightValue >= 0 && RightValue <= GetShiftSizeMask(ResultType->GetVtype()),
                        "Unexpected shift amount.");
                    if (TypeHelpers::IsUnsignedType(ResultType))
                    {
                        ResultValue = ((unsigned __int64)LeftValue >> RightValue);
                    }
                    else
                    {
                        ResultValue = LeftValue >> RightValue;
                    }
                    break;

                default:
                    VSFAIL("Surprising integral operation.");
            }

            if (Overflow)
            {
                ReportSemanticError(
                    ERRID_ExpressionOverflow1,
                    ExpressionLocation,
                    ResultType);

                return AllocateBadExpression(ExpressionLocation);
            }

            return
                ProduceConstantExpression(
                    ResultValue,
                    ExpressionLocation,
                    ResultType
                    IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
        }

        return NULL;
    }

    else if (TypeHelpers::IsFloatingType(Left->ResultType))
    {
        double LeftValue = Left->AsFloatConstantExpression().Value;
        double RightValue = Right->AsFloatConstantExpression().Value;

        if (TypeHelpers::IsBooleanType(ResultType))
        {
            bool ComparisonSucceeds = false;

            switch (Opcode)
            {
                case SX_EQ:
                    ComparisonSucceeds = LeftValue == RightValue;
                    break;
                case SX_NE:
                    ComparisonSucceeds = LeftValue != RightValue;
                    break;
                case SX_LE:
                    ComparisonSucceeds = LeftValue <= RightValue;
                    break;
                case SX_GE:
                    ComparisonSucceeds = LeftValue >= RightValue;
                    break;
                case SX_LT:
                    ComparisonSucceeds = LeftValue < RightValue;
                    break;
                case SX_GT:
                    ComparisonSucceeds = LeftValue > RightValue;
                    break;

                default:
                    VSFAIL("Surprising floating operation.");
            }

            return
                ProduceConstantExpression(
                    ComparisonSucceeds ? COMPLUS_TRUE : COMPLUS_FALSE,
                    ExpressionLocation,
                    GetFXSymbolProvider()->GetBooleanType()
                    IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
        }

        else
        {
            // Compute the result in 64-bit arithmetic, and determine if the
            // operation overflows the result type.

            double ResultValue = 0;
            bool Overflow = false;

            switch (Opcode)
            {
                case SX_ADD:
                    ResultValue = LeftValue + RightValue;
                    break;
                case SX_SUB:
                    ResultValue = LeftValue - RightValue;
                    break;
                case SX_MUL:
                    ResultValue = LeftValue * RightValue;
                    break;
                case SX_POW:
                    // VSW#463059: Special case CRT changes to match CLR behavior.
                    if(IS_DBL_INFINITY(RightValue))
                    {
                        if(IS_DBL_ONE(LeftValue))
                        {
                            ResultValue = LeftValue;
                            break;
                        }

                        if(IS_DBL_NEGATIVEONE(LeftValue))
                        {
                            *((UINT64 *)(&ResultValue)) = CLR_NAN_64;
                            break;
                        }
                    }
                    else if(_isnan(RightValue))
                    {
                        *((UINT64 *)(&ResultValue)) = CLR_NAN_64;
                        break;
                    }

                    ResultValue = pow(LeftValue, RightValue);
                    break;
                case SX_DIV:
                    // We have decided not to detect zerodivide in compile-time
                    // evaluation of floating expressions.
#if 0
                    if (RightValue == 0)
                    {
                        ReportSemanticError(
                            ERRID_ZeroDivide,
                            ExpressionLocation);

                        return AllocateBadExpression(ExpressionLocation);
                    }
#endif
                    ResultValue = LeftValue / RightValue;
                    break;
                case SX_MOD:
                    // We have decided not to detect zerodivide in compile-time
                    // evaluation of floating expressions.
#if 0
                    if (RightValue == 0)
                    {
                        ReportSemanticError(
                            ERRID_ZeroDivide,
                            ExpressionLocation);

                        return AllocateBadExpression(ExpressionLocation);
                    }
#endif
                    ResultValue = fmod(LeftValue, RightValue);
                    break;
                default:
                    VSFAIL("Surprising floating operation.");
            }

            ResultValue = NarrowFloatingResult(ResultValue, ResultType, Overflow);

            // We have decided not to detect overflow in compile-time
            // evaluation of floating expressions.
#if 0
            if (Overflow)
            {
                ReportSemanticError(
                    ERRID_ExpressionOverflow1,
                    ExpressionLocation,
                    ResultType);

                return AllocateBadExpression(ExpressionLocation);
            }
#endif
            return
                ProduceFloatingConstantExpression(
                    ResultValue,
                    ExpressionLocation,
                    ResultType
                    IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
        }

        return NULL;
    }

    else if (TypeHelpers::IsDecimalType(Left->ResultType))
    {
        DECIMAL LeftValue = Left->AsDecimalConstantExpression().Value;
        DECIMAL RightValue = Right->AsDecimalConstantExpression().Value;

        if (TypeHelpers::IsBooleanType(ResultType))
        {
            bool ComparisonSucceeds = false;
            HRESULT ComparisonResult = VarDecCmp(&LeftValue, &RightValue);

            switch (Opcode)
            {
                case SX_EQ:
                    ComparisonSucceeds = (ComparisonResult == static_cast<HRESULT>(VARCMP_EQ));
                    break;
                case SX_NE:
                    ComparisonSucceeds = !(ComparisonResult == static_cast<HRESULT>(VARCMP_EQ));
                    break;
                case SX_LE:
                    ComparisonSucceeds = (ComparisonResult == static_cast<HRESULT>(VARCMP_EQ)) ||
                                         (ComparisonResult == static_cast<HRESULT>(VARCMP_LT));
                    break;
                case SX_GE:
                    ComparisonSucceeds = (ComparisonResult == static_cast<HRESULT>(VARCMP_EQ)) ||
                                         (ComparisonResult == static_cast<HRESULT>(VARCMP_GT));
                    break;
                case SX_LT:
                    ComparisonSucceeds = (ComparisonResult == static_cast<HRESULT>(VARCMP_LT));
                    break;
                case SX_GT:
                    ComparisonSucceeds = (ComparisonResult == static_cast<HRESULT>(VARCMP_GT));
                    break;

                default:
                    VSFAIL("Surprising decimal operation.");

            }

            return
                ProduceConstantExpression(
                    ComparisonSucceeds ? COMPLUS_TRUE : COMPLUS_FALSE,
                    ExpressionLocation,
                    GetFXSymbolProvider()->GetBooleanType()
                    IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
        }

        else
        {
            DECIMAL ResultValue;
            bool Overflow = false;

            switch (Opcode)
            {
                case SX_ADD:
                    Overflow = FAILED(VarDecAdd(&LeftValue, &RightValue, &ResultValue));
                    break;
                case SX_SUB:
                    Overflow = FAILED(VarDecSub(&LeftValue, &RightValue, &ResultValue));
                    break;
                case SX_MUL:
                    Overflow = FAILED(VarDecMul(&LeftValue, &RightValue, &ResultValue));
                    break;
                case SX_DIV:
                    {
                        HRESULT hr = VarDecDiv(&LeftValue, &RightValue, &ResultValue);

                        if (hr == DISP_E_DIVBYZERO)
                        {
                            ReportSemanticError(
                                ERRID_ZeroDivide,
                                ExpressionLocation);

                            return AllocateBadExpression(ExpressionLocation);
                        }

                        Overflow = FAILED(hr);
                    }
                    break;
                case SX_MOD:
                    {
                        HRESULT hr = NOERROR;

                        // There is no VarDecMod, so we have to do this by hand
                        // result = L - (Fix(L / R) * R)

                        // 



                        hr = VarDecDiv(&LeftValue, &RightValue, &ResultValue);

                        if (!FAILED(hr))
                        {
                            hr = VarDecFix(&ResultValue, &ResultValue);

                            if (!FAILED(hr))
                            {
                                hr = VarDecMul(&ResultValue, &RightValue, &ResultValue);

                                if (!FAILED(hr))
                                {
                                    hr = VarDecSub(&LeftValue, &ResultValue, &ResultValue);
                                }
                            }
                        }

                        if (hr == DISP_E_DIVBYZERO)
                        {
                            ReportSemanticError(
                                ERRID_ZeroDivide,
                                ExpressionLocation);

                            return AllocateBadExpression(ExpressionLocation);
                        }

                        Overflow = FAILED(hr);
                    }
                    break;
                default:
                    memset(&ResultValue, 0, sizeof(DECIMAL));
                    VSFAIL("Surprising floating operation.");
            }

            if (Overflow)
            {
                ReportSemanticError(
                    ERRID_ExpressionOverflow1,
                    ExpressionLocation,
                    ResultType);

                return AllocateBadExpression(ExpressionLocation);
            }

            return
                ProduceDecimalConstantExpression(
                    ResultValue,
                    ExpressionLocation
                    IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
        }

        return NULL;
    }

    else if (TypeHelpers::IsStringType(Left->ResultType))
    {
        size_t LeftLength, RightLength;
        WCHAR *LeftSpelling, *RightSpelling;

        LeftLength = GetStringLength(Left);
        LeftSpelling = GetStringSpelling(Left);

        RightLength = GetStringLength(Right);
        RightSpelling = GetStringSpelling(Right);

        switch (Opcode)
        {
            case SX_CONC:
            {
                size_t ResultLength = LeftLength + RightLength;
                WCHAR *ResultString = new(m_TreeStorage) WCHAR[ResultLength + 1];

                memcpy(
                    ResultString,
                    LeftSpelling,
                    LeftLength * 2);
                memcpy(
                    ResultString + LeftLength,
                    RightSpelling,
                    RightLength * 2);
                ResultString[ResultLength] = 0;

                return
                    ProduceStringConstantExpression(
                        ResultString,
                        ResultLength,
                        ExpressionLocation
                        IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
            }

            case SX_GT:
            case SX_LT:
            case SX_GE:
            case SX_LE:
            case SX_EQ:
            case SX_NE:
            {
                bool StringComparisonSucceeds = false;

                int ComparisonResult =
                    (((m_SourceFileOptions & OPTION_OptionText) &&
                        !m_EvaluatingConditionalCompilationConstants) ?  // ignore Option Text when conditional compilation(b112186)
                        CompareNoCaseN :
                        CompareCaseN)(LeftSpelling, RightSpelling, min(LeftLength, RightLength));

                if (ComparisonResult == 0 && LeftLength != RightLength)
                {
                    ComparisonResult = LeftLength > RightLength ? 1 : -1;
                }

                switch (Opcode)
                {
                    case SX_EQ:
                        StringComparisonSucceeds = ComparisonResult == 0;
                        break;
                    case SX_NE:
                        StringComparisonSucceeds = ComparisonResult != 0;
                        break;
                    case SX_GT:
                        StringComparisonSucceeds = ComparisonResult > 0;
                        break;
                    case SX_GE:
                        StringComparisonSucceeds = ComparisonResult >= 0;
                        break;
                    case SX_LT:
                        StringComparisonSucceeds = ComparisonResult < 0;
                        break;
                    case SX_LE:
                        StringComparisonSucceeds = ComparisonResult <= 0;
                        break;
                }

                return
                    ProduceConstantExpression(
                        StringComparisonSucceeds ? COMPLUS_TRUE : COMPLUS_FALSE,
                        ExpressionLocation,
                        GetFXSymbolProvider()->GetBooleanType()
                        IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
            }

            default:
                VSFAIL("Surprising String operation.");
        }

    }

    else if (TypeHelpers::IsBooleanType(Left->ResultType))
    {
        Quadword LeftValue = Left->AsIntegralConstantExpression().Value;
        Quadword RightValue = Right->AsIntegralConstantExpression().Value;

        bool OperationSucceeds = false;

        switch (Opcode)
        {
            case SX_EQ:
                OperationSucceeds = LeftValue == RightValue;
                break;
            case SX_NE:
                OperationSucceeds = LeftValue != RightValue;
                break;
            // Amazingly, False > True.
            case SX_GT:
                OperationSucceeds = LeftValue == 0 && RightValue != 0;
                break;
            case SX_GE:
                OperationSucceeds = LeftValue == 0 || RightValue != 0;
                break;
            case SX_LT:
                OperationSucceeds = LeftValue != 0 && RightValue == 0;
                break;
            case SX_LE:
                OperationSucceeds = LeftValue != 0 || RightValue == 0;
                break;
            case SX_XOR:
                OperationSucceeds = (LeftValue ^ RightValue) != 0;
                break;
            case SX_ORELSE:
            case SX_OR:
                OperationSucceeds = (LeftValue | RightValue) != 0;
                break;
            case SX_ANDALSO:
            case SX_AND:
                OperationSucceeds = (LeftValue & RightValue) != 0;
                break;
            default:
                VSFAIL("Surprising boolean operation.");
        }

        return
            ProduceConstantExpression(
                OperationSucceeds ? COMPLUS_TRUE : COMPLUS_FALSE,
                ExpressionLocation,
                GetFXSymbolProvider()->GetBooleanType()
                IDE_ARG((Left->uFlags | Right->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS));
    }

    else
    {
        VSFAIL("Unimplemented compile-time operation.");
    }

    return NULL;
}

ILTree::Expression *
Semantics::PerformCompileTimeUnaryOperation
(
    BILOP Opcode,
    Type *ResultType,
    const Location &ExpressionLocation,
    ILTree::Expression *Operand
)
{
    VSASSERT(AllowsCompileTimeOperations(ResultType), "Unwelcome type for compile-time expression evaluation.");

    VSASSERT(IsConstant(Operand), "Expected a constant.");

    if (TypeHelpers::IsIntegralType(ResultType))
    {
        Quadword InputValue = Operand->AsIntegralConstantExpression().Value;
        Quadword ResultValue = 0;
        bool Overflow = false;

        switch (Opcode)
        {
            case SX_PLUS:
                ResultValue = InputValue;
                break;
            case SX_NEG:
                VSASSERT(!TypeHelpers::IsUnsignedType(ResultType), "negation of an unsigned type");
                if (InputValue == _I64_MIN)
                {
                    Overflow = true;
                }
                else
                {
                    ResultValue = -InputValue;
                }
                break;
            case SX_NOT:
                ResultValue = ~InputValue;
                break;
            default:
                VSFAIL("Surprising integral operation.");
        }

        ResultValue =
            NarrowIntegralResult(
                ResultValue,
                ResultType,
                ResultType,
                Overflow);

        if (Opcode == SX_NEG && Overflow)
        {
            ReportSemanticError(
                ERRID_ExpressionOverflow1,
                ExpressionLocation,
                ResultType);

            return AllocateBadExpression(ExpressionLocation);
        }

        return
            ProduceConstantExpression(
                ResultValue,
                ExpressionLocation,
                ResultType
                IDE_ARG(Operand->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
    }

    else if (TypeHelpers::IsFloatingType(ResultType))
    {
        double InputValue = Operand->AsFloatConstantExpression().Value;
        double ResultValue = 0;
        bool Overflow = false;

        switch (Opcode)
        {
            case SX_NEG:
                ResultValue = -InputValue;
                break;
            case SX_PLUS:
                ResultValue = InputValue;
                break;
            default:
                VSFAIL("Surprising floating operation.");
        }

        ResultValue = NarrowFloatingResult(ResultValue, ResultType, Overflow);

        // We have decided not to detect overflow in compile-time
        // evaluation of floating expressions.
#if 0
        if (Overflow)
        {
            ReportSemanticError(
                ERRID_ExpressionOverflow1,
                ExpressionLocation,
                ResultType);

            return AllocateBadExpression(ExpressionLocation);
        }
#endif
        return
            ProduceFloatingConstantExpression(
                ResultValue,
                ExpressionLocation,
                ResultType
                IDE_ARG(Operand->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
    }

    else if (TypeHelpers::IsDecimalType(ResultType))
    {
        DECIMAL InputValue = Operand->AsDecimalConstantExpression().Value;
        DECIMAL ResultValue;
        bool Overflow = false;

        switch (Opcode)
        {
            case SX_NEG:
                Overflow = FAILED(VarDecNeg(&InputValue, &ResultValue));
                break;
            case SX_PLUS:
                ResultValue = InputValue;
                break;
            default:
                memset(&ResultValue, 0, sizeof(DECIMAL));
                VSFAIL("Surprising floating operation.");
        }

        if (Overflow)
        {
            ReportSemanticError(
                ERRID_ExpressionOverflow1,
                ExpressionLocation,
                ResultType);

            return AllocateBadExpression(ExpressionLocation);
        }

        return
            ProduceDecimalConstantExpression(
                ResultValue,
                ExpressionLocation
                IDE_ARG(Operand->uFlags & SXF_CON_CONTAINS_NAMED_CONTANTS));
    }
    else if (TypeHelpers::IsBooleanType(ResultType))
    {
        VSASSERT(Opcode == SX_NOT, "Invalid state");
        return NegateBooleanExpression(Operand);
    }

    VSFAIL("Unimplemented compile-time operation");

    return NULL;
}

ILTree::Expression *
Semantics::NegateBooleanExpression
(
    ILTree::Expression *Input
)
{
    VSASSERT(
        TypeHelpers::IsBooleanType(Input->ResultType) || TypeHelpers::IsRootObjectType(Input->ResultType),
        "Boolean negation applies only to booleans.");

    // Boolean comparisons can be inverted, but Variant comparisons cannot,
    // due to some squirrelly run-time semantics.

    if (TypeHelpers::IsBooleanType(Input->ResultType))
    {
        switch (Input->bilop)
        {
            case SX_EQ:
            case SX_NE:
            case SX_LE:
            case SX_LT:
            case SX_GT:
            case SX_GE:

                // We cannot fold Negations into a floating point comparison operator
                // because the comarisons are not invertable (i.e., Op(>=) != !Op(<))

                if (!TypeHelpers::IsFloatingType(Input->AsExpressionWithChildren().Left->ResultType))
                {
                    switch (Input->bilop)
                    {
                        case SX_EQ:
                            Input->bilop = SX_NE;
                            return Input;
                        case SX_NE:
                            Input->bilop = SX_EQ;
                            return Input;
                        case SX_LE:
                            Input->bilop = SX_GT;
                            return Input;
                        case SX_LT:
                            Input->bilop = SX_GE;
                            return Input;
                        case SX_GT:
                            Input->bilop = SX_LE;
                            return Input;
                        case SX_GE:
                            Input->bilop = SX_LT;
                            return Input;
                        default:
                            VSFAIL("inconsistent logic");
                            break;
                    }
                }
                break;

            case SX_ORELSE:
                Input->bilop = SX_ANDALSO;
                Input->AsExpressionWithChildren().Left = NegateBooleanExpression(Input->AsExpressionWithChildren().Left);
                Input->AsExpressionWithChildren().Right = NegateBooleanExpression(Input->AsExpressionWithChildren().Right);
                return Input;
            case SX_ANDALSO:
                Input->bilop = SX_ORELSE;
                Input->AsExpressionWithChildren().Left = NegateBooleanExpression(Input->AsExpressionWithChildren().Left);
                Input->AsExpressionWithChildren().Right = NegateBooleanExpression(Input->AsExpressionWithChildren().Right);
                return Input;
            case SX_NOT:
                // Not Not Expr --> Expr.
                if (TypeHelpers::IsBooleanType(Input->AsExpressionWithChildren().Left->ResultType))
                {
                    return Input->AsExpressionWithChildren().Left;
                }
                break;
            case SX_CNS_INT:
                Input->AsIntegralConstantExpression().Value =
                    Input->AsIntegralConstantExpression().Value == COMPLUS_FALSE ?
                        COMPLUS_TRUE :
                        COMPLUS_FALSE;
                return Input;
        }
    }

    return
        AllocateExpression(
            SX_NOT,
            GetFXSymbolProvider()->GetBooleanType(),
            Input,
            Input->Loc);
}

size_t
GetStringLength
(
    ILTree::Expression *String
)
{
    return String->bilop == SX_NOTHING ? 0 : String->AsStringConstant().Length;
}

WCHAR *
GetStringSpelling
(
    ILTree::Expression *String
)
{
    return String->bilop == SX_NOTHING ? L"" : String->AsStringConstant().Spelling;
}

Variable *
Semantics::AllocateResultTemporary
(
    Type *ResultType
)
{
    return AllocateShortLivedTemporary(ResultType);
}

ILTree::ExpressionWithChildren *
Semantics::CaptureInTemporaryImpl
(
    ILTree::Expression *Value,
    Variable *Temporary
)
{
    ThrowIfNull(Value);
    ThrowIfNull(Temporary);

    ILTree::Expression *TemporaryReference =
        AllocateSymbolReference(
            Temporary,
            Value->ResultType,
            NULL,
            Value->Loc);
    SetFlag32(TemporaryReference, SXF_LVALUE);

    ILTree::ExpressionWithChildren *Result =
        (ILTree::ExpressionWithChildren *)AllocateExpression(
            SX_ASG,
            TypeHelpers::GetVoidType(),
            TemporaryReference,
            Value,
            Value->Loc);
    SetFlag32(Result, SXF_ASG_SUPPRESS_CLONE);

    return Result;
}

ILTree::Expression *
Semantics::CaptureInTemporaryAsSequenceImpl
(
    ILTree::Expression *Value,
    Variable *Temporary,
    ILTree::ExpressionWithChildren *Assign
)
{
    ThrowIfNull(Temporary);
    ThrowIfNull(Assign);

    // Why is it OK to return the assignment node for object reference
    // types and not for other types? (This rule comes from the old analyzer.)

    if (TypeHelpers::IsClassOrInterfaceType(Value->ResultType))
    {
        SetResultType(Assign, Value->ResultType);
        return Assign;
    }

    ILTree::Expression *TemporaryReference =
        AllocateSymbolReference(Temporary, Value->ResultType, NULL, Value->Loc);

    // If the sequence operator ends up being fed to UseTwice, having this
    // temporary reference marked as an LValue prevents generating an
    // additional (spurious) temporary.
    SetFlag32(TemporaryReference, SXF_LVALUE);

    return
        AllocateExpression(
            SX_SEQ_OP2,
            Value->ResultType,
            Assign,
            TemporaryReference,
            Value->Loc);
}

ILTree::Expression *
Semantics::CaptureInAddressedTemporary
(
    ILTree::Expression *Argument,
    Type *TemporaryType,
    Variable *&Temporary
)
{
    VSASSERT(
        TypeHelpers::EquivalentTypes(Argument->ResultType, TemporaryType) ||
            (TypeHelpers::IsStringType(TemporaryType) && TypeHelpers::IsStringType(Argument->ResultType)) ||
            (IsOrInheritsFromOrImplements(Argument->ResultType, TemporaryType)),
        "Type mismatch in capturing to a temporary.");

    Temporary =
        AllocateResultTemporary(TemporaryType);

    ILTree::Expression *TemporaryReference =
        AllocateSymbolReference(
            Temporary,
            TemporaryType,
            NULL,
            Argument->Loc);
    SetFlag32(TemporaryReference, SXF_LVALUE);

    Argument =
        AllocateExpression(
            SX_ASG_RESADR,
            GetPointerType(TemporaryType),
            TemporaryReference,
            Argument,
            Argument->Loc);

    return Argument;
}

void
Semantics::UseTwiceImpl
(
    ILTree::Expression *Value,
    ILTree::Expression *&FirstResult,
    ILTree::Expression *&SecondResult,
    bool UseLongLivedTemporaries,
    bool FirstResultUsedAsValue,
    ILTree::ExecutableBlock *block
)
{
    ThrowIfFalse(!UseLongLivedTemporaries || block); // Using LongLived Temporaries requires a block

    if (Value == NULL)
    {
        SecondResult = NULL;

        if (FirstResultUsedAsValue)
        {
            FirstResult = NULL;
        }

        return;
    }

    switch (Value->bilop)
    {
        // A twice-used argument can end up being passed byref, in which
        // case the two uses are supposed to be aliased. This requires creating
        // temporaries for all RValues, even those without side effects.

#if 0
        case SX_CNS_FLT:
        case SX_CNS_DEC:
        case SX_CNS_INT:
        case SX_CNS_STR:
        case SX_NOTHING:

            SecondResult = &m_TreeAllocator.xCopyBilNode(Value)->AsExpression();
            FirstResult = Value;

            break;
#endif
        case SX_ASG:
        case SX_ASG_RESADR:

            // If these are assigning to a temporary, then the correct treatment
            // is to make the second use a reference to the temporary.
            // If these are assigning to a real variable, this
            // treatment seems inappropriate.
            // The compiler is inferring that an assignment is a temporary
            // capture if and only if the result type of the assignment is not
            // void. This seems unsound.

            if (Value->AsExpressionWithChildren().Left->bilop == SX_SYM &&
                !TypeHelpers::IsVoidType(Value->ResultType))
            {
                UseTwiceImpl(
                    Value->AsExpressionWithChildren().Left,
                    FirstResultUsedAsValue ?
                        FirstResult->AsExpressionWithChildren().Left :
                        FirstResult,
                    SecondResult,
                    UseLongLivedTemporaries,
                    true,
                    block);

                if (Value->bilop == SX_ASG_RESADR)
                {
                    SecondResult = MakeAddress(SecondResult, true);
                }

                break;
            }

            __fallthrough;

        case SX_INDEX:
        case SX_LIST:
        case SX_IF:
        case SX_VARINDEX:
        case SX_PROPERTY_REFERENCE:
        case SX_LATE_REFERENCE:
        case SX_LATE:
        case SX_SEQ:
        case SX_SEQ_OP1:
        case SX_SEQ_OP2:

            if (FirstResultUsedAsValue)
            {
                FirstResult = Value;
            }

            if (Value->bilop == SX_SEQ_OP2)
            {
                if(Value->AsExpressionWithChildren().Left->bilop == SX_ASG ||
                   Value->AsExpressionWithChildren().Left->bilop == SX_INIT_STRUCTURE)
                {
                    // In a non-value-producing context, the second use
                    // of an assignment can be omitted.
                    // In this case, the second result
                    // is the other operand of the sequence operator.

                    // This assumes that the sequence operator is being used
                    // to capture a temporary, which seems unsound.

                    UseTwiceImpl(
                        Value->AsExpressionWithChildren().Right,
                        FirstResultUsedAsValue ?
                            FirstResult->AsExpressionWithChildren().Right :
                            FirstResult,
                        SecondResult,
                        UseLongLivedTemporaries,
                        FirstResultUsedAsValue,
                        block);

                    break;
                }
        else if((Value->AsExpressionWithChildren().Left->bilop == SX_CALL ||
                           Value->AsExpressionWithChildren().Left->bilop == SX_SEQ ) &&     // 
                        TypeHelpers::IsVoidType(Value->AsExpressionWithChildren().Left->ResultType) &&
                        Value->AsExpressionWithChildren().Right->bilop == SX_SYM &&
                        !TypeHelpers::IsVoidType(Value->ResultType) &&
                        !FirstResult)
                {
                    // 
                Variable *Temporary = NULL;

                    FirstResult = UseLongLivedTemporaries ?
                        CaptureInLongLivedTemporary(Value, Temporary, block) :
                        CaptureInShortLivedTemporary(Value, Temporary);

                    // Produce a second reference to the temporary.

                    SecondResult =
                        AllocateSymbolReference(Temporary, Value->ResultType, NULL, Value->Loc);

                    // The second result may end up being passed Byref.
                    SetFlag32(SecondResult, SXF_LVALUE);

                    break;
                }
            }

            if (Value->bilop == SX_SEQ_OP1 &&
                (Value->AsExpressionWithChildren().Right->bilop == SX_ASG ||
                 Value->AsExpressionWithChildren().Right->bilop == SX_INIT_STRUCTURE))
            {
                // In a non-value-producing context, the second use
                // of an assignment can be omitted.
                // In this case, the second result
                // is the other operand of the sequence operator.

                // This assumes that the sequence operator is being used
                // to capture a temporary, which seems unsound.

                UseTwiceImpl(
                    Value->AsExpressionWithChildren().Left,
                    FirstResultUsedAsValue ?
                        FirstResult->AsExpressionWithChildren().Left :
                        FirstResult,
                    SecondResult,
                    UseLongLivedTemporaries,
                    FirstResultUsedAsValue,
                    block);

                break;
            }

            SecondResult = &m_TreeAllocator.xCopyBilNode(Value)->AsExpression();

            UseTwiceImpl(
                Value->AsExpressionWithChildren().Left,
                FirstResultUsedAsValue ?
                    FirstResult->AsExpressionWithChildren().Left :
                    FirstResult,
                SecondResult->AsExpressionWithChildren().Left,
                UseLongLivedTemporaries,
                FirstResultUsedAsValue,
                block);

            UseTwiceImpl(
                Value->AsExpressionWithChildren().Right,
                FirstResultUsedAsValue ?
                    FirstResult->AsExpressionWithChildren().Right :
                    FirstResult,
                SecondResult->AsExpressionWithChildren().Right,
                UseLongLivedTemporaries,
                FirstResultUsedAsValue,
                block);

            break;

        case SX_ARG:
        case SX_ADR:

            SecondResult = &m_TreeAllocator.xCopyBilNode(Value)->AsExpression();

            if (FirstResultUsedAsValue)
            {
                FirstResult = Value;
            }

            UseTwiceImpl(
                Value->AsExpressionWithChildren().Left,
                FirstResultUsedAsValue ?
                    FirstResult->AsExpressionWithChildren().Left :
                    FirstResult,
                SecondResult->AsExpressionWithChildren().Left,
                UseLongLivedTemporaries,
                FirstResultUsedAsValue,
                block);

            
            // Dev11 










            if (Value->bilop == SX_ADR && 
                FirstResultUsedAsValue && 
                FirstResult->AsExpressionWithChildren().Left != nullptr && 
                FirstResult->AsExpressionWithChildren().Left->bilop == SX_SEQ_OP2 && 
                FirstResult->AsExpressionWithChildren().Left->AsExpressionWithChildren().Left != nullptr &&
                FirstResult->AsExpressionWithChildren().Left->AsExpressionWithChildren().Left->bilop == SX_ASG)
            {
                FirstResult = FirstResult->AsExpressionWithChildren().Left;
            }

            break;

        case SX_SYM:

            // If the symbol reference is an lvalue (or a procedure), then the
            // two uses must both refer to this symbol. Otherwise, the value
            // of the symbol must be captured. (Readonly variables and Me
            // are valid either way. Copying the symbol reference is more
            // efficient than capturing the value.)

            if (HasFlag32(Value, SXF_LVALUE) ||
                !Value->AsSymbolReferenceExpression().Symbol->IsVariable() ||
                Value->AsSymbolReferenceExpression().Symbol->PVariable()->IsReadOnly() ||
                Value->AsSymbolReferenceExpression().Symbol->PVariable()->IsMe())
            {
                SecondResult = &m_TreeAllocator.xCopyBilNode(Value)->AsExpression();

                if (FirstResultUsedAsValue)
                {
                    FirstResult = Value;
                }

                UseTwiceImpl(
                    Value->AsSymbolReferenceExpression().BaseReference,
                    FirstResultUsedAsValue ?
                        FirstResult->AsSymbolReferenceExpression().BaseReference :
                        FirstResult,
                    SecondResult->AsSymbolReferenceExpression().BaseReference,
                    UseLongLivedTemporaries,
                    FirstResultUsedAsValue,
                    block);

                break;
            }

            __fallthrough;

        case SX_CALL:

            // Normally if we have a method that returns type T, we store the value of the call
            // into a temp of type T and create a SymbolReferenceExpression of the temp for 
            // SecondResult. If the method is void-returning however, it doesn't make sense to 
            // create a temp of type void. Therefore, we simply make the call in FirstResult and
            // do nothing in the SecondResult, thus avoiding any sideeffects of the call from happening
            // twice. This is Dev11 

            if (Value->bilop == SX_CALL && //Guard against fallthrough from previous cases
                TypeHelpers::IsVoidType(Value->ResultType))
            {
                if (FirstResultUsedAsValue)
                {
                    FirstResult = Value;
                }

                SecondResult = NULL;

                break;
            }
            __fallthrough;

        default:
        {
            Variable *Temporary = NULL;

            if (FirstResultUsedAsValue)
            {
                FirstResult = UseLongLivedTemporaries ?
                    CaptureInLongLivedTemporaryAsSequence(Value, block) :
                    CaptureInShortLivedTemporaryAsSequence(Value);

                // The captured value may be used as a Byref argument, in which case
                // the address of the temporary is to be passed. The LValue flag
                // makes recognizing this case possible.

                SetFlag32(FirstResult, SXF_LVALUE);

                VSASSERT(
                    FirstResult->bilop == SX_SEQ_OP2 || FirstResult->bilop == SX_ASG,
                    "Temporary capture produced an unexpected tree.");

                // Extract the temporary from the capture expression.

                Temporary =
                    FirstResult->bilop == SX_SEQ_OP2 ?
                        FirstResult->AsExpressionWithChildren().Left->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->PVariable() :
                        FirstResult->AsExpressionWithChildren().Left->AsSymbolReferenceExpression().Symbol->PVariable();
            }
            else
            {
                ILTree::ExpressionWithChildren *Capture = UseLongLivedTemporaries ?
                    CaptureInLongLivedTemporary(Value, Temporary, block) :
                    CaptureInShortLivedTemporary(Value, Temporary);

                if (FirstResult == NULL)
                {
                    FirstResult = Capture;
                }
                else
                {
                    FirstResult =
                        AllocateExpression(
                            SX_SEQ,
                            TypeHelpers::GetVoidType(),
                            FirstResult,
                            Capture,
                            Capture->Loc);
                }
            }

            // Produce a second reference to the temporary.

            SecondResult =
                AllocateSymbolReference(Temporary, Value->ResultType, NULL, Value->Loc);

            // The second result may end up being passed Byref.
            SetFlag32(SecondResult, SXF_LVALUE);

            break;
        }
    }
}

bool
Semantics::IsSimplePropertyGet
(
    Declaration *Property,
    GenericBinding *PropertyGenericBindingContext,
    Type *AccessingInstanceType,
    bool PropertyIsTargetOfAssignment,
    const Location &SourceLocation

)
{
    bool Result = false;
    bool SomeOverloadsBad = false;

    do
    {
        for (Declaration *NextProcedure = Property;
             NextProcedure;
             NextProcedure = NextProcedure->GetNextOverload())
        {
            // Amazingly, non-procedures can land here if a class defines both fields
            // and methods with the same name. (This is impossible in VB, but apparently
            // possible for classes written in other languages.)

            if (!IsProperty(NextProcedure) ||
                !IsAccessible(NextProcedure, PropertyGenericBindingContext, AccessingInstanceType))
            {
                continue;
            }

            Procedure *Get = ViewAsProcedure(NextProcedure)->PProperty()->GetProperty();

            if (Get == NULL)
            {
                // This property is Writeonly. In the context of an assignment, an
                // accessible Set method with no Get makes it impossible to treat
                // the property reference as a simple Get.

                if (PropertyIsTargetOfAssignment)
                {
                    return false;
                }

                continue;
            }

            Procedure *NonAliasProcedure = ViewAsProcedure(Get);

            // This method is an accessible property Get.

            if (NonAliasProcedure->GetParameterCount() > 0)
            {
                // If there are any accessible properties with parameters that have
                // Get methods, this isn't a simple Get.

                return false;
            }

            // This method is an accessible property Get with no parameters.

            Result = true;
        }

    } while (Property = FindMoreOverloadedProcedures(Property, AccessingInstanceType, SourceLocation, SomeOverloadsBad));

    return SomeOverloadsBad ? false : Result;
}

Variable *
Semantics::CreateImplicitDeclaration
(
    _In_z_ Identifier *Name,
    typeChars TypeCharacter,
    Location *loc,
    ExpressionFlags Flags,
    bool lambdaMember
)
{
    Type *VariableType;
    DECLFLAGS VarDeclFlags = DECLF_NotDecled | DECLF_Public;

    if (TypeCharacter == chType_NONE)
    {
        VariableType = GetFXSymbolProvider()->GetObjectType();
        VarDeclFlags |= DECLF_NotTyped;
    }
    else
    {
        VariableType = GetFXSymbolProvider()->GetType(VtypeOfTypechar(TypeCharacter));
    }

    Variable *Result;

    Result = m_SymbolCreator.AllocVariable(loc != NULL, false);

    m_SymbolCreator.GetVariable(
        loc,
        Name,
        Name,
        VarDeclFlags,
        VAR_Local,
        VariableType,
        NULL,
        NULL,
        Result);

    if( OptionInferOn() && m_CreateExplicitScopeForLoop > 0 )
    {
        // Fix for 

        if (HasFlag(Flags, ExprInferLoopControlVariableExplicit))
        {
            AssertIfNull( loc );
        }

        Scope *ForLocals =
            m_SymbolCreator.GetHashTable(
                NULL,
                m_Lookup,
                true,
                m_CreateExplicitScopeForLoop,
                NULL);

        Symbols::AddSymbolToHash(
            ForLocals,
            Result,
            true,
            false,
            false);

        m_Lookup = ForLocals;
        m_CreateExplicitScopeForLoop = 0;
        m_ExplicitLoopVariableCreated = true;

        Result->SetImplicitDecl( false );
    }
    else
    {
        BCSYM_Hash *pScope = NULL;

        // The implicit variable should be put into the lambda's hash if it is
        // in a field level lambda.
        // 
        // Otherwise place it in the procedure's hash.

        VSASSERT( (m_StatementLambdaInterpreter && m_OuterStatementLambdaTree) || ((m_StatementLambdaInterpreter == NULL) && (m_OuterStatementLambdaTree == NULL)),
                  "If we have a multiline lambda tree we must have an outer most multiline lambda tree, which may be the same lambda. Otherwise, both should be NULL");

        if (m_StatementLambdaInterpreter && 
            !(m_OuterStatementLambdaTree->Locals->GetParent() &&
            m_OuterStatementLambdaTree->Locals->GetParent()->IsProc()))
        {
            pScope = m_OuterStatementLambdaTree->Locals;
        }
        else if (m_ProcedureTree)
        {
            pScope = m_ProcedureTree->Locals;
        }
        else 
        {
            pScope = m_Lookup;
        }
        
        Symbols::AddSymbolToHash(
            pScope,
            Result,
            true,
            false,
            false);
    }

    Result->SetIsUsed();
    Result->SetIsLambdaMember(m_StatementLambdaInterpreter != NULL);

    return Result;
}

ILTree::ExtensionCallExpression *
Semantics::AllocateExtensionCall
(
    ILTree::Expression * BaseReference,
    ExtensionCallLookupResult * ExtensionCallLookupResult,
    const Location & TreeLocation,
    unsigned ImplicitMeErrorID,
    bool SynthesizedMeReference
)
{
    ThrowIfNull(BaseReference);

    ILTree::ExtensionCallExpression * Result = (ILTree::ExtensionCallExpression *)m_TreeAllocator.xAllocBilNode(SX_EXTENSION_CALL);

    ExpressionListHelper helper(this);

    helper.Add
    (
        AllocateExpression
        (
            SX_ARG,
            TypeHelpers::GetVoidType(),
            BaseReference,
            BaseReference->Loc
        ),
        TreeLocation
    );

    Result->ImplicitArgumentList = helper.Start();

    Result->ResultType = TypeHelpers::GetVoidType();
    Result->Loc = TreeLocation;
    Result->ExtensionCallLookupResult = ExtensionCallLookupResult;
    Result->ImplicitMeErrorID = ImplicitMeErrorID;


    if (SynthesizedMeReference)
    {
        SetFlag32(Result, SXF_EXTENSION_CALL_ME_IS_SYNTHETIC);
    }

    return Result;
}

ILTree::DeferredTempExpression * 
Semantics::AllocateDeferredTemp
(
    ParseTree::Expression *InitialValue,
    Type *ResultType,
    ExpressionFlags ExprFlags,
    const Location & TreeLocation
)
{
    ILTree::DeferredTempExpression * Result = (ILTree::DeferredTempExpression *)m_TreeAllocator.xAllocBilNode(SX_DEFERRED_TEMP);
    m_methodDeferredTempCount++;

    // If m_methodDeferredTempCount is 0 after incrementing, an overflow occurred.
    ThrowIfTrue(m_methodDeferredTempCount == 0);
    
    Result->Id = m_methodDeferredTempCount;
    Result->InitialValue = InitialValue;
    Result->ResultType = ResultType;
    Result->InterpretFlags = ExprFlags;
    Result->Loc = TreeLocation;

    return Result;
}


ILTree::Expression *
Semantics::AllocateExpression
(
    BILOP Opcode,
    Type *ResultType,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    const Location &StartLocation,
    const Location &EndLocation
)
{
    ILTree::Expression *Result = (ILTree::Expression *)m_TreeAllocator.xAllocSxTree(Opcode, Left, Right);

    Result->Loc.m_lBegLine = StartLocation.m_lBegLine;
    Result->Loc.m_lBegColumn = StartLocation.m_lBegColumn;

    Result->Loc.m_lEndLine = EndLocation.m_lEndLine;
    Result->Loc.m_lEndColumn = EndLocation.m_lEndColumn;

    SetResultType(Result, ResultType);

    return Result;
}

ILTree::Expression *
Semantics::AllocateExpression
(
    BILOP Opcode,
    Type *ResultType,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    const Location &TreeLocation
)
{
    ILTree::Expression *Result = (ILTree::Expression *)m_TreeAllocator.xAllocSxTree(Opcode, Left, Right);

    Result->Loc = TreeLocation;

    SetResultType(Result, ResultType);

    return Result;
}

ILTree::Expression *
Semantics::AllocateUserDefinedOperatorExpression
(
    BILOP Opcode,
    Type *ResultType,
    ILTree::Expression *Left,
    ILTree::Expression *Right,
    const Location &TreeLocation
)
{
    ILTree::Expression *Result = (ILTree::Expression *)m_TreeAllocator.xAllocUDOSxTree(Opcode, Left, Right);

    Result->Loc = TreeLocation;

    SetResultType(Result, ResultType);

    return Result;
}

ILTree::Expression *
Semantics::AllocateIIfExpression
(
    Type *ResultType,
    ILTree::Expression *Condition,
    ILTree::Expression *TruePart,
    ILTree::Expression *FalsePart,
    const Location &TreeLocation
)
{
    ILTree::Expression *Result = (ILTree::Expression *)m_TreeAllocator.xAllocSxTree(SX_IIF, TruePart, FalsePart);

    Result->Loc = TreeLocation;
    Result->AsIfExpression().condition = Condition;

    SetResultType(Result, ResultType);

    return Result;
}

ILTree::Expression *
Semantics::AllocateBadExpression
(
    const Location &TreeLocation
)
{
    return AllocateBadExpression(TypeHelpers::GetVoidType(), TreeLocation);
}

ILTree::Expression *
Semantics::AllocateBadExpression
(
    Type *ResultType,
    const Location &TreeLocation
)
{
    if (ResultType == NULL)
    {
        ResultType = TypeHelpers::GetVoidType();
    }
    ILTree::Expression *Result = AllocateExpression(SX_BAD, ResultType, TreeLocation);
    return MakeBad(Result);
}

ILTree::DelegateConstructorCallExpression *
Semantics::AllocateDelegateConstructorCall
(
    Type *ResultType,
    ILTree::Expression *Constructor,
    ILTree::Expression *ObjectArgument,
    ILTree::Expression *Method,
    const Location &TreeLocation
)
{
    ILTree::DelegateConstructorCallExpression *Result = (ILTree::DelegateConstructorCallExpression *)m_TreeAllocator.xAllocBilNode(SX_DELEGATE_CTOR_CALL);

    Result->Constructor = Constructor;
    Result->ObjectArgument = ObjectArgument;
    Result->Method = Method;

    Result->Loc = TreeLocation;

    SetResultType(Result, ResultType);

    return Result;
}

ILTree::SymbolReferenceExpression *
Semantics::AllocateSymbolReference
(
    Declaration *Symbol,
    Type *ResultType,
    ILTree::Expression *BaseReference,
    const Location &TreeLocation,
    GenericBinding *GenericBindingContext
)
{
    ILTree::SymbolReferenceExpression *Result = (ILTree::SymbolReferenceExpression *)m_TreeAllocator.xAllocBilNode(SX_SYM);

    Result->BaseReference = BaseReference;
    Result->Symbol = Symbol;
    Result->GenericBindingContext = GenericBindingContext;

    Result->Loc = TreeLocation;

    SetResultType(Result, ResultType);

    return Result;
}

BILOP
Semantics::MapOperator
(
    ParseTree::Expression::Opcodes Opcode
)
{
    switch (Opcode)
    {
        case ParseTree::Expression::UnaryPlus:      return SX_PLUS;
        case ParseTree::Expression::Negate:         return SX_NEG;
        case ParseTree::Expression::Not:            return SX_NOT;
        case ParseTree::Expression::Plus:           return SX_ADD;
        case ParseTree::Expression::Minus:          return SX_SUB;
        case ParseTree::Expression::Multiply:       return SX_MUL;
        case ParseTree::Expression::Divide:         return SX_DIV;
        case ParseTree::Expression::Power:          return SX_POW;
        case ParseTree::Expression::IntegralDivide: return SX_IDIV;
        case ParseTree::Expression::Concatenate:    return SX_CONC;
        case ParseTree::Expression::Modulus:        return SX_MOD;
        case ParseTree::Expression::Or:             return SX_OR;
        case ParseTree::Expression::OrElse:         return SX_ORELSE;
        case ParseTree::Expression::Xor:            return SX_XOR;
        case ParseTree::Expression::And:            return SX_AND;
        case ParseTree::Expression::AndAlso:        return SX_ANDALSO;
        case ParseTree::Expression::Like:           return SX_LIKE;
        case ParseTree::Expression::Is:             return SX_IS;
        case ParseTree::Expression::IsNot:          return SX_ISNOT;
        case ParseTree::Expression::Equal:          return SX_EQ;
        case ParseTree::Expression::NotEqual:       return SX_NE;
        case ParseTree::Expression::Less:           return SX_LT;
        case ParseTree::Expression::LessEqual:      return SX_LE;
        case ParseTree::Expression::GreaterEqual:   return SX_GE;
        case ParseTree::Expression::Greater:        return SX_GT;
        case ParseTree::Expression::ShiftLeft:      return SX_SHIFT_LEFT;
        case ParseTree::Expression::ShiftRight:     return SX_SHIFT_RIGHT;
        default:
            VSFAIL("Surprising operator opcode.");
    }

    return SX_BAD;
}

STRING *
Semantics::GetDefaultInstanceBaseNameForMyGroupMember
(
    BCSYM_Class *Class, //in
    bool    *MangleName //out
)
{
    if (!m_Project || Class->IsEnum())
        return NULL;

    ULONG count = m_Project->m_daMyGroupCollectionInfo.Count();
    if (!count)
        return NULL;

    STRING* name = NULL;
    bool fSeen = false;
    bool fMangle = false;

    MyGroupCollectionInfo* arrayOfMyGroupCollectionInfo = m_Project->m_daMyGroupCollectionInfo.Array();
    for (unsigned i = 0 ; i < count; i++)
    {
        BCSYM_Class *GroupClass = arrayOfMyGroupCollectionInfo[i].m_groupClass;
        WellKnownAttrVals::MyGroupCollectionData* groupCollectionData;
        GroupClass->GetPWellKnownAttrVals()->GetMyGroupCollectionData(&groupCollectionData);

        WellKnownAttrVals::MyGroupCollectionBase* arrayBase = groupCollectionData->Array();
        bool hasDefaultInstance = false;
        for (unsigned mi = 0 ; mi < groupCollectionData->Count(); mi++)
        {
            if (arrayBase[mi].m_DefaultInstance)
            {
                hasDefaultInstance = true;
                break;
            }
        }

        if (hasDefaultInstance)
        {
            BCSYM_Class** arrayOfClassesInMyGroupMember = arrayOfMyGroupCollectionInfo[i].m_daMyGroupMembers.Array();
            ULONG countMem = arrayOfMyGroupCollectionInfo[i].m_daMyGroupMembers.Count();
            for (unsigned k = 0 ; k < countMem; k++)
            {
                BCSYM_Class *memberClass = arrayOfClassesInMyGroupMember[k];
                if (BCSYM::AreTypesEqual(memberClass , Class))
                {
                    unsigned int index;
                    if (!Bindable::FindBaseInMyGroupCollection(Class, groupCollectionData, &index))
                    {
                        VSFAIL("Bad group member class, no base found");
                        return NULL;
                    }
                    if (arrayBase[index].m_DefaultInstance)
                    {
                        if (fSeen)
                        {
#if DEBUG
                            VSDEBUGPRINTIF(
                                VSFSWITCH(fMyGroupAndDefaultInst),
                                "MyGroup: failed to produce a default instance because the class '%S' was found as member in multiple 'my' groups\n",
                                Class->GetName());
#endif
                            return NULL;
                        }
                        name = arrayBase[index].m_DefaultInstance;
                        fSeen = true;
                        // the array is sorted, members with the same simple name are grouped together.
                        // if two adjacent members with the same simple name then use name mangling
                        fMangle =
                        ( k > 0 &&
                            StringPool::IsEqual(memberClass->GetName(), arrayOfClassesInMyGroupMember[k-1]->GetName())) ||
                        (k < countMem-1 &&
                            StringPool::IsEqual(memberClass->GetName() , arrayOfClassesInMyGroupMember[k+1]->GetName()));

                    }
                }
            }
        }
    }

    if (MangleName)
    {
        *MangleName = fMangle;
    }

    return name;
}

inline ILTree::Expression*
Semantics::CheckForDefaultInstanceProperty
(
    const Location& ReferringLocation,
    ILTree::Expression* BaseReference,
    _In_z_ STRING* MyBaseName,
    ExpressionFlags Flags,
    bool MangleName
)
{
    // "foo.a" is interpreted as "My.MyProject.Forms.foo.a" when "foo" is a type that is member of My group Forms
    // and the group has in the MyGroupCollection attribute the default instance argument 'My.MyProject.Forms'

    if ( !m_Project || !MyBaseName || !BaseReference || IsBad(BaseReference) ||
        BaseReference->bilop != SX_SYM || !BaseReference->ResultType->IsClass() ||
        BaseReference->ResultType->IsEnum())
    {
#if DEBUG
        VSDEBUGPRINTIF(
            VSFSWITCH(fMyGroupAndDefaultInst),
            "MyGroup: failed to produce a default instance because the base expression is '%S'\n",
            ( !m_Project || !MyBaseName || !BaseReference || IsBad(BaseReference) ||
            BaseReference->bilop != SX_SYM || !BaseReference->ResultType->IsClass()) ? L" bad" : L" enum");
#endif
        return NULL;
    }

    NorlsAllocator  Scratch(NORLSLOC);
    STRING *className = BaseReference->ResultType->PClass()->GetName();
    StringBuffer MyNameText;
    MyNameText.AppendString(MyBaseName);
    MyNameText.AppendChar(L'.');
    if (MangleName)
    {
        MyNameText.AppendString(Bindable::GetMyGroupScrambledName(BaseReference->ResultType->PClass(), &Scratch, m_Compiler));
    }
    else
    {
        MyNameText.AppendString(className);
    }

    Parser NameParser(
        &m_TreeStorage,
        m_Compiler,
        m_CompilerHost,
        false,
        m_Project->GetCompilingLanguageVersion()
        );

    Scanner
        Scanner(
        m_Compiler,
        MyNameText.GetString(),
        MyNameText.GetStringLength(),
        0,
        BaseReference->Loc.m_lBegLine,
        BaseReference->Loc.m_lBegColumn);

    ParseTree::Expression *MyDefInstParseTree;
    bool ErrorInConstructRet = false;
    NameParser.
        ParseOneExpression(
            &Scanner,
            NULL,
            //m_Errors,
            &MyDefInstParseTree,
            &ErrorInConstructRet);

    if (!MyDefInstParseTree || ErrorInConstructRet )
    {
#if DEBUG
        VSDEBUGPRINTIF(
            VSFSWITCH(fMyGroupAndDefaultInst),
            "MyGroup: failed to produce a default instance because the default instance expression '%S' cannot be parsed\n",
            MyNameText.GetString());
#endif
        return NULL;
    }
    bool ReportErrors = m_ReportErrors;
    m_ReportErrors = false;

    // try to fixup locations for as much of the tree as possible
    MyDefInstParseTree->TextSpan.SetLocation(BaseReference);
    for (ParseTree::Expression *currentTree = MyDefInstParseTree ;
         currentTree->Opcode == ParseTree::Expression::DotQualified;
         currentTree = currentTree->AsQualified()->Base)
    {
        currentTree->AsQualified()->Base->TextSpan.SetLocation(BaseReference);
        VSASSERT(currentTree->AsQualified()->Name != NULL,"QualifiedExpression::Name must be set!");
        currentTree->AsQualified()->Name->TextSpan.SetLocation(BaseReference);
    }


    ExpressionFlags OperandFlags = Flags |
                                   ExprSuppressImplicitVariableDeclaration |
                                   ExprSuppressDefaultInstanceSynthesis;
    ClearFlag(OperandFlags, ExprAccessDefaultProperty);

    ILTree::Expression* Result = InterpretExpression(
        MyDefInstParseTree,
        OperandFlags);

    m_ReportErrors = ReportErrors;

    // if the default inst expression cannot be correctly bound to an instance of the same type as the class
    // ignore it
    if (!Result || IsBad(Result) ||
        !BCSYM::AreTypesEqual(BaseReference->ResultType->PClass(),Result->ResultType))
    {
#if DEBUG
        VSDEBUGPRINTIF(
            VSFSWITCH(fMyGroupAndDefaultInst),
            "MyGroup: failed to produce a default instance because the default instance expression '%S' cannot be interpreted\n",
            MyNameText.GetString());
#endif
        return NULL;
    }

    if (m_Procedure &&
        !m_Procedure->IsShared() &&
        BCSYM::AreTypesEqual(m_Procedure->GetContainingClass(), BaseReference->ResultType->PClass()))
    {
        ReportSemanticError(
            ERRID_CantReferToMyGroupInsideGroupType1,
            ReferringLocation,
            BaseReference->ResultType->PClass());
    }

    return Result;
}


int
GetShiftSizeMask
(
    Vtypes Type
)
{
    switch (Type)
    {
        case t_i1:
        case t_ui1:
            return 0x7;
        case t_i2:
        case t_ui2:
            return 0xF;
        case t_i4:
        case t_ui4:
            return 0x1F;
        case t_i8:
        case t_ui8:
            return 0x3F;
        default:
            VSFAIL("unexpected shift type!");
            return 0xBADBADFF;
    }
}

typeChars
ExtractTypeCharacter
(
    ParseTree::Expression *Input
)
{
    switch (Input->Opcode)
    {
        case ParseTree::Expression::Name:
            return Input->AsName()->Name.TypeCharacter;
        case ParseTree::Expression::DotQualified:
        case ParseTree::Expression::BangQualified:
            return ExtractTypeCharacter(Input->AsQualified()->Name);
        case ParseTree::Expression::GenericQualified:
            return ExtractTypeCharacter(Input->AsGenericQualified()->Base);
    }

    return chType_NONE;
}

Procedure *
MatchesPropertyRequirements
(
    Procedure *Property,
    ExpressionFlags Flags
)
{
    VSASSERT(IsProperty(Property), "Advertised property method isn't.");

    if (HasFlag(Flags, ExprIsPropertyAssignment))
    {
        return Property->PProperty()->SetProperty();
    }

    return Property->PProperty()->GetProperty();
}

Procedure *
ResolveOverriddenProperty
(
    Procedure *Target,
    ExpressionFlags Flags,
    bool &ResolvedToDifferentTarget
)
{
    VSASSERT(IsProperty(Target) && Target->IsOverrides(), "expected overridden property");

    Procedure *Current = Target;
    do
    {
        if (MatchesPropertyRequirements(Current, Flags))
        {
            if (Current != Target) ResolvedToDifferentTarget = true;
            return Current;
        }
        Current = Current->OverriddenProcLast();
    }
    while (Current);

    return Target;
}

void
Semantics::ReportPropertyMismatch
(
    Declaration *AllegedProperty,
    ExpressionFlags Flags,
    const Location &ErrorLocation
)
{
    if (AllegedProperty->IsProperty() &&
        AllegedProperty->GetContainer()->IsAnonymousType() &&
        AllegedProperty->PProperty()->GetProperty() == NULL &&
        AllegedProperty->PProperty()->SetProperty() == NULL)
    {
        ReportSemanticError(
            ERRID_AnonymousTypePropertyOutOfOrder1,
            ErrorLocation,
            AllegedProperty->GetErrorName(m_Compiler));
    }
    else
    {
        ReportSemanticError(
            HasFlag(Flags, ExprIsPropertyAssignment) ?
                ERRID_NoSetProperty1 :
                ERRID_NoGetProperty1,
            ErrorLocation,
            // Use just the property's name, not its full description.
            // Fetch the name explicitly to avoid having the full description of the
            // incorrect property appear in the message.
            AllegedProperty->GetErrorName(m_Compiler));
    }
}

void
Semantics::ReportBadAwaitInNonAsync
(
    const Location &loc
)
{
    if (m_InLambda)
    {
        ReportSemanticError(ERRID_BadAwaitInNonAsyncLambda, loc);
    }
    else if (m_Procedure != NULL && (m_Procedure->GetType() != NULL))
    {
        if (!m_Procedure->GetType()->IsVoidType())
        {
            ReportSemanticError(ERRID_BadAwaitInNonAsyncMethod, loc, m_Procedure->GetType());
        }
        else
        {
            ReportSemanticError(ERRID_BadAwaitInNonAsyncVoidMethod, loc);
        }
    }
    else
    {
        ReportSemanticError(ERRID_BadAwaitNotInAsyncMethodOrLambda, loc);
    }
}

ILTree::Expression *Semantics::InterpretAttribute
(
    ParseTree::Expression *ExpressionTree,
    ParseTree::ArgumentList *NamedArguments,
    Scope *Lookup,
    Declaration *NamedContextOfAppliedAttribute,
    ClassOrRecordType *AttributeClass,
    Location *Location
)
{
    ILTree::Expression *Result = NULL;
    bool BadArg = false;

    InitializeInterpretationState(
        NULL,
        Lookup,
        NULL,
        NULL,       // No call graph.
        m_SymbolsCreatedDuringInterpretation,
        false,      // preserve extra semantic information
        true,       // perform obsolete checks
        false,      // cannot interpret statements
        true,       // can interpret expressions
        m_MergeAnonymousTypeTemplates); // merge anonymous type templates

    // The context in which the attribute was applied.
    //      - If the attribute was applied on a container,
    //        then the the context is the container itself
    //      - If the attribute was appiled on a namedroot,
    //        the the context is the container of the named
    //        root.
    //      - If the attribute was appiled on a parameter,
    //        then the context is the method the parameter
    //        belongs to
    // Why do we need this ? - This is needed because bindable
    // processes the attributes on a container as part of
    // the container itself unlike other named roots for which
    // the attributes are processed as part of their container.
    //
    // Bindable does this because of the ----ymmetry between
    // attributes on namespaces where the lookup and the use
    // context are the same vs. on other types where the lookup
    // is the parent of the use context.
    //
    // This needs to be passed to the obsolete checker
    // which in turn can attach any delayed obsolete checks
    // to the bindable instance of this container.
    //
    // Store the previous and set the new context for Obsolete checks
    //
    // NOTE: if returning before the end of the procedure, make sure
    // the m_NamedContextForAppliedAttribute is restored.
    //

    VSASSERT(m_NamedContextForAppliedAttribute == NULL, "Recursive attribute evaluation unexpected!!!");

    m_NamedContextForAppliedAttribute = NamedContextOfAppliedAttribute;
    m_CreateImplicitDeclarations = false;

    // Get the bound tree for a call to the constructor.
    ILTree::Expression *ConstructorCall =
        InterpretExpression(
            ExpressionTree,
            ExprIsConstructorCall | ExprForceConstructorCall | ExprResultNotNeeded | ExprArgumentsMustBeConstant | ExprSuppressDefaultInstanceSynthesis);

    if (IsBad(ConstructorCall))
    {
        m_NamedContextForAppliedAttribute = NULL;
        return AllocateBadExpression(*Location);
    }

    Procedure *Constructor = ConstructorCall->AsCallExpression().Left->AsSymbolReferenceExpression().Symbol->PProc();

    // Make sure the constructor is public
    if (Constructor->GetAccess() != ACCESS_Public)
    {
        ReportSemanticError(
            ERRID_BadAttributeNonPublicConstructor,
            *Location);
    }

    // Check that all formal parameters have attribute-compatible types and are public
    for (Parameter *CurrentParameter = Constructor->GetFirstParam();
         CurrentParameter;
         CurrentParameter = CurrentParameter->GetNext())
    {
        Type *ParameterType = CurrentParameter->GetType();

        if (!IsValidAttributeType(ParameterType, m_CompilerHost))
        {
            BadArg = true;
            if( ParameterType->IsPointerType() )
            {
                // an attempt was made here to report a byref arg in an attribute ctor not at the call
                // location but at the definition location. This would break the case when the attribute is
                // comming from metadata and has no location. see b115739. Restored the original location.
                ReportSemanticError(
                    ERRID_BadAttributeConstructor2,
                    //CurrentParameter->GetLocation(),
                    *Location,
                    ParameterType);
            }
            else
            {
                ReportSemanticError(
                    ERRID_BadAttributeConstructor1,
                    *Location,
                    ParameterType);
            }
        }

        // The class and all of its containers must be public.  We only need to check this for
        // enums because all other legal attribute types are system primitives, and are
        // therefore guaranteed not to be nested.
        if (TypeHelpers::IsEnumType(ParameterType))
        {
            // 


            Declaration *EnumType = ParameterType->PNamedRoot();

            if (EnumType->GetAccess() != ACCESS_Public)
            {
                BadArg = true;
                ReportSemanticError(
                    ERRID_BadAttributeNonPublicType1,
                    *Location,
                    EnumType);
            }
            else
            {
                // Check all containers.
                for (Declaration *Parent = EnumType->GetParent();
                     Parent;
                     Parent = Parent->GetParent())
                {
                    if (Parent->GetAccess() != ACCESS_Public)
                    {
                        BadArg = true;
                        ReportSemanticError(
                            ERRID_BadAttributeNonPublicContType2,
                            *Location,
                            EnumType,
                            Parent);
                        break;
                    }
                }
            }
        }
    }

    //
    // Walk the list of named parameters if any and process them.
    //

    ILTree::Expression *MappedNamedArguments = NULL;
    ILTree::Expression *CurrentMappedArgument = NULL;

    for (; NamedArguments; NamedArguments = NamedArguments->Next)
    {
        Identifier *Name = NamedArguments->Element->Name.Name;

        if (Name == NULL)
        {
            // There was a syntax error -- an unnamed argument follows a
            // named argument. Ignore this argument.

            BadArg = true;
            continue;
        }

        bool NameIsBad;

        Declaration *FieldOrProperty =

            EnsureNamedRoot
            (
                InterpretName
                (
                    Name,
                    ViewAsScope(AttributeClass),
                    NULL,   // No Type parameter lookup
                    NameSearchIgnoreParent | NameSearchIgnoreExtensionMethods,
                    NULL,
                    NamedArguments->Element->Name.TextSpan,
                    NameIsBad,
                    NULL,  // No generic attributes, so expect no binding context
                    -1
                )
            );

        if (!NameIsBad && FieldOrProperty == NULL)
        {
            ReportSemanticError(
                ERRID_PropertyOrFieldNotDefined1,
                NamedArguments->Element->Name.TextSpan,
                Name);

            NameIsBad = true;
        }

        if (!NameIsBad &&
            !(FieldOrProperty->IsVariable() || IsProperty(FieldOrProperty)))
        {
            ReportSemanticError(
                ERRID_AttrAssignmentNotFieldOrProp1,
                NamedArguments->Element->Name.TextSpan,
                FieldOrProperty);

            NameIsBad = true;
        }

        // If we found a property symbol, search for the non-index property.
        if (!NameIsBad && FieldOrProperty && FieldOrProperty->IsProperty())
        {
            Declaration *OverloadedMember = FieldOrProperty;
            FieldOrProperty = NULL;

            do
            {
                Declaration *CurrentMember = OverloadedMember;

                do
                {
                    if(CurrentMember->IsProperty() &&
                       !CurrentMember->PProperty()->GetParameterCount())
                    {
                        if (IsAccessible(
                                CurrentMember,
                                NULL,   // No generic attributes
                                NULL))
                        {
                            FieldOrProperty = CurrentMember;
                            goto FoundMatchingProperty;
                        }
                        else if (!FieldOrProperty)
                        {
                            FieldOrProperty = CurrentMember;
                        }
                    }

                    CurrentMember = CurrentMember->GetNextBound();
                }
                while (CurrentMember);

                OverloadedMember = FindMoreOverloadedProcedures(OverloadedMember, NULL, NamedArguments->Element->Name.TextSpan, NameIsBad);
                AssertIfTrue(NameIsBad);
            }
            while (OverloadedMember);

FoundMatchingProperty:

            if (!FieldOrProperty)
            {
                ReportSemanticError(
                    ERRID_NoNonIndexProperty1,
                    NamedArguments->Element->Name.TextSpan,
                    Name);

                NameIsBad = true;
            }
            else if (!FieldOrProperty->PProperty()->SetProperty())
            {
                ReportSemanticError(
                    ERRID_ReadOnlyProperty1,
                    NamedArguments->Element->TextSpan,
                    FieldOrProperty->GetName());

                NameIsBad = true;
            }
            else
            {
                CheckAccessibility(
                    FieldOrProperty->PProperty()->SetProperty(),
                    NULL,   // No generic attributes
                    NamedArguments->Element->Name.TextSpan,
                    NameSearchIgnoreParent,
                    NULL,
                    NameIsBad);
            }
        }

        // Verify that the field or property we are trying to assign to has a
        // valid attribute parameter type, storage specifier, and access specifier.
        if (!NameIsBad)
        {
            BCSYM_Member *Member = FieldOrProperty->PMember();
            Symbol * Type = Member->GetType()->DigThroughAlias();

            // Named properties/fields can only have certain types
            if (!IsValidAttributeType(Type, m_CompilerHost))
            {
                ReportSemanticError(
                    ERRID_BadAttributePropertyType1,
                    NamedArguments->Element->Name.TextSpan,
                    FieldOrProperty->GetName());
                NameIsBad = true;
            }

            // Named properties/fields cannot be shared
            if (Member->IsShared())
            {
                ReportSemanticError(
                    ERRID_BadAttributeSharedProperty1,
                    NamedArguments->Element->Name.TextSpan,
                    FieldOrProperty->GetName());
                NameIsBad = true;
            }

            // Named properties/fields cannot be read-only
            if ((Member->IsProperty() && Member->PProperty()->IsReadOnly()) ||
                (Member->IsVariable() && Member->PVariable()->IsReadOnly()))
            {
                ReportSemanticError(
                    ERRID_BadAttributeReadOnlyProperty1,
                    NamedArguments->Element->Name.TextSpan,
                    FieldOrProperty->GetName());
                NameIsBad = true;
            }

            // Named properties/fields cannot be const
            if (Member->IsVariable() && Member->PVariable()->IsConstant())
            {
                ReportSemanticError(
                    ERRID_BadAttributeConstField1,
                    NamedArguments->Element->Name.TextSpan,
                    FieldOrProperty->GetName());
                NameIsBad = true;
            }

            // Named properties/fields must be public
            if ( Member->GetAccess() != ACCESS_Public ||
                 ( Member->IsProperty() &&
                   FieldOrProperty->PProperty()->SetProperty()->GetAccess() != ACCESS_Public))
            {
                ReportSemanticError(
                    ERRID_BadAttributeNonPublicProperty1,
                    NamedArguments->Element->Name.TextSpan,
                    FieldOrProperty->GetName());
                NameIsBad = true;
            }
        }

        if (NameIsBad)
        {
            BadArg = true;
        }
        else
        {
            ILTree::Expression *Value =
                InterpretExpressionWithTargetType(
                    NamedArguments->Element->Value,
                    ExprForceRValue | ExprMustBeConstant,
                    FieldOrProperty->PMember()->GetType());

            if (IsBad(Value))
            {
                BadArg = true;
            }
            else
            {
                ILTree::SymbolReferenceExpression * FieldOrPropertyReference =
                    AllocateSymbolReference(
                        FieldOrProperty,
                        FieldOrProperty->PMember()->GetType(),
                        NULL,
                        NamedArguments->Element->Name.TextSpan);

                ILTree::Expression * Assignment =
                    AllocateExpression(
                        SX_ASG,
                        FieldOrProperty->PMember()->GetType(),
                        FieldOrPropertyReference,
                        Value,
                        NamedArguments->Element->TextSpan);

                ILTree::Expression * AssignmentListNode =
                    AllocateExpression(
                        SX_LIST,
                        FieldOrProperty->PMember()->GetType(),
                        Assignment,
                        NULL,
                        NamedArguments->Element->TextSpan);

                if (MappedNamedArguments == NULL)
                {
                    MappedNamedArguments = CurrentMappedArgument = AssignmentListNode;
                }
                else
                {
                    CurrentMappedArgument->AsExpressionWithChildren().Right = AssignmentListNode;
                    CurrentMappedArgument = CurrentMappedArgument->AsExpressionWithChildren().Right;
                }

            }

        }
    }

    m_NamedContextForAppliedAttribute = NULL;

    if (BadArg)
    {
        return AllocateBadExpression(*Location);
    }

    //
    // Finally generate the bound tree!
    //

    Result =
        AllocateExpression(
            SX_APPL_ATTR,
            TypeHelpers::GetVoidType(),
            ConstructorCall,
            MappedNamedArguments,
            *Location);

    return Result;
}

bool
Semantics::IsObsoleteStandaloneExpressionKeyword
(
    _In_z_ Identifier *Name
)
{
    // Has the user used an obsolete keyword?  If so, give an intelligent error.

    switch (Compiler::TokenOfString(Name))
    {
        case tkEMPTY:
        case tkNULL:
        case tkRND:

            return true;
    }

    return false;
}


// This function builds a LIST tree from the bottom up by traversing the
// Concatenation tree depth-first from the right hand side
//
ExpressionList *
Semantics::BuildConcatList
(
    ILTree::Expression *CurrentNode,
    ExpressionList *BuiltTree,
    unsigned &ElementCount
)
{
    if (CurrentNode->bilop != SX_CONC)
    {
        ElementCount++;

        return
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                CurrentNode,
                BuiltTree,
                CurrentNode->Loc);
    }
    else
    {
        return
            BuildConcatList(
                CurrentNode->AsExpressionWithChildren().Left,
                BuildConcatList(
                    CurrentNode->AsExpressionWithChildren().Right,
                    BuiltTree,
                    ElementCount),
                ElementCount);
    }
}

// Walk the concat list and combine contiguous runs of constant strings, modifying
// the list in-place.
//
void
Semantics::ReduceConcatList
(
    ExpressionList *ConcatList,
    unsigned &ElementCount
)
{
    while (ConcatList)
    {
        if (ConcatList->AsExpressionWithChildren().Left->bilop == SX_CNS_STR)
        {
            ExpressionList *Current = ConcatList;
            ExpressionList *Previous = NULL;
            size_t ResultLength = 0;

            do
            {
                ResultLength += Current->AsExpressionWithChildren().Left->AsStringConstant().Length;
                Previous = Current;
                Current = Current->AsExpressionWithChildren().Right;
            }
            while (Current && Current->AsExpressionWithChildren().Left->bilop == SX_CNS_STR);

            // If the string constant run has more than one item, then collapse it.
            if (Current != ConcatList->AsExpressionWithChildren().Right)
            {
                WCHAR *ResultString = new(m_TreeStorage) WCHAR[ResultLength + 1];
                ResultString[ResultLength] = 0;

                size_t WrittenLength = 0;

                Current = ConcatList;
                do
                {
                    IfFalseThrow(ResultString + WrittenLength >= ResultString);
                    memcpy(
                        ResultString + WrittenLength,
                        Current->AsExpressionWithChildren().Left->AsStringConstant().Spelling,
                        Current->AsExpressionWithChildren().Left->AsStringConstant().Length * sizeof(WCHAR));
                    IfFalseThrow((WrittenLength += Current->AsExpressionWithChildren().Left->AsStringConstant().Length) >= Current->AsExpressionWithChildren().Left->AsStringConstant().Length);
                    Current = Current->AsExpressionWithChildren().Right;
                    ElementCount--;
                }
                while (Current && Current->AsExpressionWithChildren().Left->bilop == SX_CNS_STR);

                VSASSERT(WrittenLength == ResultLength, "String literal concatenation confused.");

                ConcatList->AsExpressionWithChildren().Left->AsStringConstant().Spelling = ResultString;
                ConcatList->AsExpressionWithChildren().Left->AsStringConstant().Length = ResultLength;
                ConcatList->AsExpressionWithChildren().Right = Current;
                ConcatList = Current;
                ElementCount++;
                continue;
            }
        }

        ConcatList = ConcatList->AsExpressionWithChildren().Right;
    }
}

// Select the optimal overload of System.String.Concat.
//
// For OperandCount == 2, use Concat(String, String)
// For OperandCount == 3, use Concat(String, String, String)
// For OperandCount == 4, use Concat(String, String, String, String)
// For OperandCount >= 5, use Concat(String[])
//
ILTree::Expression *
Semantics::OptimizeConcatenate
(
    ILTree::Expression *ConcatTree,
    const Location &ConcatLocation
)
{
    ILTree::Expression *Result = ConcatTree;

    // If we have only one concat, then do nothing and let codegen generate the normal two parameter concat.
    //
    if (ConcatTree->AsExpressionWithChildren().Left->bilop  == SX_CONC ||
        ConcatTree->AsExpressionWithChildren().Right->bilop == SX_CONC)
    {
        unsigned OperandCount = 0;
        ExpressionList *ArgumentList = NULL;

        // Build a LIST of operands from the Concat tree
        ExpressionList *ConcatList =
            BuildConcatList(
                ConcatTree,
                NULL,
                OperandCount);

        // Collapse contiguous runs of string constants.
        ReduceConcatList(ConcatList, OperandCount);

        VSASSERT(OperandCount >= 2,
                 "How can concat list reduce to one operand? This should have been covered by constant expression evaluation.");

        if (OperandCount == 2 || OperandCount == 3 || OperandCount == 4)
        {
            // We can use the 3 or 4-string-parameter overload of concat.  Take the LIST generated above
            // and insert ARG nodes at each element.
            //
            // Also, every concat considers its operands to be implicitly coerced to string.
            // Therefore, force the arguments to string.  The runtime helper will ensure the necessary
            // VB conversion behavior.

            ILTree::Expression **Element1 = &ConcatList->AsExpressionWithChildren().Left;
            ILTree::Expression **Element2 = &ConcatList->AsExpressionWithChildren().Right->AsExpressionWithChildren().Left;

            *Element1 =
                AllocateExpression(
                    SX_ARG,
                    TypeHelpers::GetVoidType(),
                    ConvertWithErrorChecking(
                        *Element1,
                        GetFXSymbolProvider()->GetStringType(),
                        ExprForceRValue),
                    ConcatLocation);

            *Element2 =
                AllocateExpression(
                    SX_ARG,
                    TypeHelpers::GetVoidType(),
                    ConvertWithErrorChecking(
                        *Element2,
                        GetFXSymbolProvider()->GetStringType(),
                        ExprForceRValue),
                    ConcatLocation);

            if (OperandCount >= 3)
            {
                ILTree::Expression **Element3 = &ConcatList->AsExpressionWithChildren().Right->AsExpressionWithChildren().Right->AsExpressionWithChildren().Left;

                *Element3 =
                    AllocateExpression(
                        SX_ARG,
                        TypeHelpers::GetVoidType(),
                        ConvertWithErrorChecking(
                            *Element3,
                            GetFXSymbolProvider()->GetStringType(),
                            ExprForceRValue),
                        ConcatLocation);

                if (OperandCount == 4)
                {
                    ILTree::Expression **Element4 = &ConcatList->AsExpressionWithChildren().Right->AsExpressionWithChildren().Right->AsExpressionWithChildren().Right->AsExpressionWithChildren().Left;

                    *Element4 =
                        AllocateExpression(
                            SX_ARG,
                            TypeHelpers::GetVoidType(),
                            ConvertWithErrorChecking(
                                *Element4,
                                GetFXSymbolProvider()->GetStringType(),
                                ExprForceRValue),
                            ConcatLocation);
                }
            }

            // This modified tree becomes the ARG list for the call to concat.
            ArgumentList = ConcatList;
        }
        else
        {
            // We can use the String-array overload of concat, so now we will generate that array, initialized
            // with all of the concat operands.

            ArrayType *ConcatArrayType = m_SymbolCreator.GetArrayType(1, GetFXSymbolProvider()->GetStringType());

            ILTree::Expression *ConcatArray =
                InitializeArray(
                    ConcatList,
                    ConcatArrayType,
                    NULL,
                    ConcatLocation);

            ArgumentList =
                AllocateExpression(
                    SX_LIST,
                    TypeHelpers::GetVoidType(),
                    AllocateExpression(
                        SX_ARG,
                        TypeHelpers::GetVoidType(),
                        ConcatArray,
                        ConcatLocation),
                    NULL,
                    ConcatLocation);
        }

        Procedure *ConcatMethod =
            FindHelperMethod(STRING_CONST(m_Compiler, Concat), GetFXSymbolProvider()->GetStringType()->PClass(), ConcatLocation);

        if (ConcatMethod == NULL)
        {
            return AllocateBadExpression(ConcatLocation);
        }

        // Use the argument list to bind to the correct overload of concat.
        Result =
            InterpretCallExpressionWithNoCopyout(
                ConcatLocation,
                AllocateSymbolReference(
                    ConcatMethod,
                    TypeHelpers::GetVoidType(),
                    NULL,
                    ConcatLocation),
                chType_NONE,
                ArgumentList,
                false,
                ExprNoFlags,
                NULL);
    }


    return Result;
}

ILTree::Expression*
Semantics::AlterForMyGroup
(
    ILTree::Expression *Operand,
    Location location
)
{
    if (Operand->bilop == SX_CALL && Operand->AsCallExpression().Left->bilop == SX_SYM &&
        Operand->AsCallExpression().Left->AsSymbolReferenceExpression().pnamed->IsProc() &&
        Operand->AsCallExpression().Left->AsSymbolReferenceExpression().pnamed->PProc()->IsPropertyGet() &&
        Operand->AsCallExpression().Left->AsSymbolReferenceExpression().pnamed->PProc()->IsMyGenerated())
    {
        ILTree::Expression *TmpResult = NULL;
        ILTree::Expression  *BaseReference = Operand->AsCallExpression().MeArgument;
        STRING* supportField = m_Compiler->ConcatStrings(
            CLS_MYGROUPCOLLECTION_FIELD_PREFIX,
            Operand->AsCallExpression().Left->AsSymbolReferenceExpression().pnamed->PProc()->GetAssociatedPropertyDef()->GetName());

        ParseTree::IdentifierDescriptor fieldId =
        {
            supportField,
                chType_NONE,
                false,
                false
        };
        fieldId.TextSpan = location;

        // interpret "BaseReference.fieldId",
        TmpResult =
            InterpretQualifiedExpression(
                BaseReference,      // do not make a copy, it is safe to use BaseReference. It is the name TmpResult or to be thown out otherwise
                supportField,
                ParseTree::Expression::DotQualified,
                fieldId.TextSpan,
                ExprForceRValue);

        if (TmpResult && !IsBad(TmpResult) &&
            TmpResult->bilop == SX_SYM &&
            TmpResult->AsSymbolReferenceExpression().pnamed->IsVariable() &&
            TmpResult->AsSymbolReferenceExpression().pnamed->PVariable()->IsMyGenerated())
        {
            Operand = TmpResult;
        }
        else
        {
            VSFAIL("MyGroup IS/ISNOT special semantics fails");
        }
    }
    return Operand;
}

ParseTree::IdentifierDescriptor *
Semantics::ExtractName
(
    ParseTree::Expression *Input,
    bool &IsNameBangQualified
)
{
    switch (Input->Opcode)
    {
        case ParseTree::Expression::Name:
            return &Input->AsName()->Name;
        case ParseTree::Expression::XmlName:
            return &Input->AsXmlName()->LocalName;
        case ParseTree::Expression::DotQualified:
        case ParseTree::Expression::BangQualified:
        case ParseTree::Expression::XmlElementsQualified:
        case ParseTree::Expression::XmlAttributeQualified:
        case ParseTree::Expression::XmlDescendantsQualified:
            IsNameBangQualified = Input->Opcode == ParseTree::Expression::BangQualified;
            return ExtractName(Input->AsQualified()->Name, IsNameBangQualified);
        case ParseTree::Expression::GenericQualified:
            return ExtractName(Input->AsGenericQualified()->Base, IsNameBangQualified);
        case ParseTree::Expression::CallOrIndex:
            return ExtractName(Input->AsCallOrIndex()->Target, IsNameBangQualified);
    }

    return NULL;
}

ILTree::Expression *
Semantics::CreateInitializedObject
(
    ParseTree::BracedInitializerList *BracedInitializerList,
    ILTree::Expression *ObjectToInitialize,
    const Location &TextSpanOfObjectInit,
    const Location &TextSpanOfWithClause,
    ExpressionFlags Flags
)
{
    AssertIfNull(ObjectToInitialize);

    if (NULL == BracedInitializerList)
    {
        return AllocateBadExpression(ObjectToInitialize->ResultType, TextSpanOfObjectInit);
    }

    if (NULL == BracedInitializerList->InitialValues)
    {
        return ObjectToInitialize;
    }

    Variable *TempVar =NULL;
    ILTree::Expression *TemporaryAssignment = CaptureInShortLivedTemporary(ObjectToInitialize, TempVar);

    ILTree::Expression *ObjectToInitializeRef =
        AllocateSymbolReference(
            TempVar,
            TempVar->GetType(),
            NULL,
            ObjectToInitialize->Loc);

    // Indicate that the object expression is an LValue and that it can be used
    // in the LHS.
    SetFlag32(ObjectToInitializeRef, SXF_LVALUE);

    ILTree::Expression *InitializationList =
        InitializeObject(
            BracedInitializerList,
            ObjectToInitializeRef,
            TextSpanOfWithClause,
            Flags);

    AssertIfNull(InitializationList);

    if (IsBad(InitializationList))
    {
#if IDE
        if (m_IsGeneratingXML)
        {
            // 





            return MakeBad(ObjectToInitialize);
        }
#endif IDE

        return AllocateBadExpression(ObjectToInitialize->ResultType, TextSpanOfObjectInit);
    }

    return
        AllocateExpression(
            SX_SEQ_OP2,
            TempVar->GetType(),
            AllocateExpression(
                SX_SEQ,
                TypeHelpers::GetVoidType(),
                TemporaryAssignment,
                InitializationList,
                TextSpanOfObjectInit),
            AllocateSymbolReference(
                TempVar,
                TempVar->GetType(),
                NULL,
                ObjectToInitialize->Loc,
                NULL),
            TextSpanOfObjectInit);
}

ILTree::Expression *
Semantics::InitializeObject
(
    ParseTree::BracedInitializerList *BracedInitializerList,
    ILTree::Expression *ObjectToInitialize,
    const Location &TextSpanOfWithClause,
    ExpressionFlags Flags
)
{
    AssertIfNull(ObjectToInitialize);
    AssertIfNull(BracedInitializerList);

    if (TypeHelpers::EquivalentTypes(ObjectToInitialize->ResultType, GetFXSymbolProvider()->GetObjectType()))
    {
        // Aggregate initializer syntax cannot be used to initialize an instance of type 'Object'.
        ReportSemanticError(ERRID_AggrInitInvalidForObject, TextSpanOfWithClause);
        return AllocateBadExpression(ObjectToInitialize->ResultType, TextSpanOfWithClause);
    }

    ExpressionList *InitializeList = NULL;
    ExpressionList *InitializationList = NULL;
    ILTree::Expression **InitializationTarget = &InitializationList;

    ILTree::WithBlock *PrevEnclosingWith = m_EnclosingWith;
    m_EnclosingWith = &AllocateStatement(SB_WITH, ObjectToInitialize->Loc, 0, false)->AsWithBlock();
    m_EnclosingWith->ObjectBeingInitialized = ObjectToInitialize;
    SetFlag32(m_EnclosingWith, SBF_WITH_LVALUE);


    ExistanceTree<STRING_INFO *> InitializedMembers;
    InitializedMembers.Init(&m_TreeStorage);    // Rather than create a new allocator, use the
                                                // Tree allocated which is anyway relatively
                                                // short lived i.e. the lifetime of one method's
                                                // Analysis only.

    bool SomeInitializerIsBad = false;
    for (ParseTree::InitializerList *Initializers = BracedInitializerList->InitialValues;
         Initializers;
         Initializers = Initializers->Next)
    {
        ParseTree::Initializer *Operand = Initializers->Element;
        ParseTree::IdentifierDescriptor *FieldName = NULL;
        ILTree::Expression *Value = NULL;

        AssertIfNull(Operand);
        AssertIfFalse(ParseTree::Initializer::Assignment == Operand->Opcode);

        FieldName = &Operand->AsAssignment()->Name;

        if (FieldName->IsBad)
        {
            SomeInitializerIsBad = true;
            continue;
        }

        // We defer evaluating the initializer until we know the type
        Location OperandTextSpan = Operand->AsAssignment()->TextSpan;

        AssertIfNull(FieldName);
        AssertIfNull(FieldName->Name);

        // Get the case independent token for the string
        STRING_INFO *FiledNameKey = StringPool::Pstrinfo(FieldName->Name);

        if (InitializedMembers.Add(FiledNameKey))   // If Node already exists, "Add" returns "true" without adding,
                                                    // else adds the Key
        {
            // Cannot initialize the same Field/Property multiple times.
            ReportSemanticError(ERRID_DuplicateAggrMemberInit1, FieldName->TextSpan, FieldName->Name);
            SomeInitializerIsBad = true;
            continue;
        }

        ParseTree::NameExpression Name;
        Name.Opcode = ParseTree::Expression::Name;
        Name.Name = *FieldName;
        Name.TextSpan = FieldName->TextSpan;

        ILTree::Expression *Target = NULL;
        {
            BackupValue<Type *> backup_receiver_type(&m_pReceiverType); //this variable will restore the value of
                                                                        //m_pReceiverType when it goes out of scope.
            BackupValue<Location *> backup_receiver_location(&m_pReceiverLocation);

            ILTree::Expression *BaseObjectExpr = EnclosingWithValue(FieldName->TextSpan);

            //Save the result type of the base reference so that extension method
            //name lookup can use it to apply the super-type filter to extension methods.
            m_pReceiverType = BaseObjectExpr->ResultType;
            m_pReceiverLocation = &BaseObjectExpr->Loc;

            Target =
                InterpretQualifiedExpression(
                    BaseObjectExpr,
                    &Name,
                    ParseTree::Expression::DotQualified,
                    FieldName->TextSpan,
                    ExprIsAssignmentTarget | ExprPropagatePropertyReference | ExprIsLHSOfObjectInitializer);

            Target =
                Target ?
                    ApplyContextSpecificSemantics(Target, ExprIsAssignmentTarget | ExprPropagatePropertyReference, NULL) :
                    NULL;

            if (!Target || IsBad(Target) )
            {
                SomeInitializerIsBad = true;
                continue;
            }
        }
        AssertIfNull(Target);

        if (Operand && Operand->Opcode == ParseTree::Initializer::Assignment)
        {
            // OK, now we can evaluate the initializer
            Value =
                InterpretInitializer(
                    Operand->AsAssignment()->Initializer,
                    (Type *)NULL,
                    ExprForceRValue | ExprDontInferResultType); // We don't want to infer a result type because it is guaranteed to come from LHS.
        }

        if (NULL == Value || IsBad(Value))
        {
            SomeInitializerIsBad = true;
            continue;
        }

        ILTree::Expression *MemberInitialization =
            GenerateAssignment(
                OperandTextSpan,
                Target,
                IsPropertyReference(Target) ?
                    Value :     // Don't convert yet because the final target member that will
                                // be bound to is not known for overloaded properties till after
                                // overload resolution.
                    ConvertWithErrorChecking(Value, Target->ResultType, ExprForceRValue | ExprGetLambdaReturnTypeFromDelegate),
                false,
                true);  // "true" indicates that this is an assignment in an aggregate initializer

        *InitializationTarget =
            AllocateExpression(
                SX_LIST,
                TypeHelpers::GetVoidType(),
                MemberInitialization,
                NULL,
                MemberInitialization->Loc);
        InitializationTarget = &(*InitializationTarget)->AsExpressionWithChildren().Right;
    }

    m_EnclosingWith = PrevEnclosingWith;

    return
        SomeInitializerIsBad?
            AllocateBadExpression(TextSpanOfWithClause) :
            InitializationList;
}

ILTree::Expression *
Semantics::InterpretCallOrIndex
(
    ParseTree::CallOrIndexExpression * CallOrIndex,
    ExpressionFlags Flags,
    typeChars TypeCharacter
)
{

    ILTree::Expression *Result = NULL;
    ILTree::Expression *BaseReference =
        InterpretExpression
        (
            CallOrIndex->Target,
            (
                ExprIsExplicitCallTarget |
                ExprAccessDefaultProperty |
                ExprPropagatePropertyReference |
                ExprSuppressImplicitVariableDeclaration |
                (
                    (Flags & ExprIsConstructorCall) ?
                        (
                            ExprIsConstructorCall |
                            (
                                (Flags & ExprForceConstructorCall) ?
                                    ExprTypeReferenceOnly :
                                    0
                            )
                        ) :
                        0
                )
            )
        );


    bool BaseReferenceCalledWithoutArguments = false;

    // There is an old VB rule $11.8 to the effect that an expression of the form
    // "func(arguments)" calls func with arguments if func takes arguments, and
    // calls func without arguments and applies indexing to the result if func
    // takes no arguments. Dev10#487076: This requires care in the presence of overloading;
    // it works only when there is exactly one accessible candidate, and this
    // candidate takes zero arguments. 
    // Note: the following code has awful structure. That's because it's layer upon
    // layer of patches and bugfixes. It deserves to be rewritten more cleanly.

    if
    (
        !IsBad(BaseReference) &&
        CallOrIndex->Arguments.Values &&
        (
            (
                BaseReference->bilop == SX_SYM &&
                IsProcedure(BaseReference->AsSymbolReferenceExpression().Symbol)
            ) ||
            (
                BaseReference->bilop == SX_EXTENSION_CALL &&
                BaseReference->AsExtensionCallExpression().ExtensionCallLookupResult->CanApplyDefaultPropertyTransformation()
            )
        )
    )
    {
        Declaration *TargetProcedure = NULL;  // this variable isn't actually used except as a boolean.
        
        if (BaseReference->bilop == SX_SYM)
        {
            VSASSERT(IsProcedure(BaseReference->AsSymbolReferenceExpression().Symbol), "internal logic error: we allow either SX_SYM/IsProcedure, or SX_EXTENSION_CALL/CanApplyDefaultPropertyTransformation");
            Procedure *FirstCandidateProcedure = ViewAsProcedure(BaseReference->AsSymbolReferenceExpression().Symbol);

            if (!FirstCandidateProcedure->IsOverloads() && FirstCandidateProcedure->GetParameterCount()==0 &&
                !IsSub(FirstCandidateProcedure))
            {
                TargetProcedure = FirstCandidateProcedure;
                // that was a quick shortcut, to save us having to resolve overload candidates
            }
            else if (FirstCandidateProcedure->IsOverloads())
            {
                unsigned int CandidateCount=0, RejectedForArgumentCount=0, RejectedForTypeArgumentCount=0;
                bool ResolutionFailed=false; NorlsAllocator Scratch(NORLSLOC);

                OverloadList * Candidates =
                    CollectOverloadCandidates
                    (
                        NULL, // ExistingCandidateList
                        FirstCandidateProcedure,
                        BaseReference->AsSymbolReferenceExpression().GenericBindingContext,
                        NULL, // Arguments
                        0,    // ArgumentCount
                        NULL, // DelegateReturnType
                        NULL, // TypeArguments
                        0,    // TypeArgumentCount
                        Flags,
                        OvrldExactArgCount | OvrldIgnoreEvents | OvrldIgnoreSubs,
                        InstanceTypeOfReference(BaseReference->AsSymbolReferenceExpression().BaseReference), // AccessingInstanceType
                        Scratch,
                        CandidateCount, // out
                        RejectedForArgumentCount, // out
                        RejectedForTypeArgumentCount, // out
                        BaseReference->Loc,
                        ResolutionFailed, // out
                        NULL // this parameter is for ambiguous asyncs. But when we're doing default-prop-transform, that doesn't apply
                );

                // NB. You might ask why we're collecting overload candidates here for an instance method,
                // but not also collecting overload candidates in case of an extension method.
                // The answer is that, if the thing was resolved as an extension method call, then
                // (1) the extension method is necessarily accessible, and (2) it's already been
                // dealt with (more elegantly) in the SX_EXTENSION_CALL case above.
    
                if (!ResolutionFailed &&
                    CandidateCount==1 && RejectedForArgumentCount==0 && RejectedForTypeArgumentCount==0 &&
                    IsProcedure(Candidates->Candidate) && !IsSub(ViewAsProcedure(Candidates->Candidate)))
                {
                    TargetProcedure = Candidates->Candidate;
                }
            }
        }

        if
        (
            BaseReference->bilop == SX_EXTENSION_CALL ||
            TargetProcedure != NULL ||
            // !!! danger. The following IsSimplyPropertyGet relies on the assumption that
            // IsProcedure(BaseReference->AsSymbolReferenceExpression().Symbol).
            // That assumption is true only through short-circuiting, i.e. the fact that
            // if bilop!=SX_EXTENSION_CALL then we have bilop==SX_SYM && IsProcedure. Yuck.
            IsSimplePropertyGet
            (
                BaseReference->AsSymbolReferenceExpression().Symbol,
                BaseReference->AsSymbolReferenceExpression().GenericBindingContext,
                InstanceTypeOfReference(BaseReference->AsSymbolReferenceExpression().BaseReference),
                HasFlag(Flags, ExprIsAssignmentTarget),
                BaseReference->Loc
            )
        )
        {
            // Don't apply this transformation if the argument
            // list is a single open parenthesis. (Doing so leads to confusing
            // error messages, and prevents Intellisense from displaying tips
            // for calls to methods without parameters.)

            if (CallOrIndex->Arguments.ClosingParenthesisPresent ||
                CallOrIndex->Arguments.Values->Next ||
                CallOrIndex->Arguments.Values->Element->Name.Name ||
                (CallOrIndex->Arguments.Values->Element->Value &&
                 CallOrIndex->Arguments.Values->Element->Value->Opcode != ParseTree::Expression::SyntaxError))
            {
                BaseReference =
                    ApplyContextSpecificSemantics(
                        InterpretCallExpressionWithNoCopyout(
                            BaseReference->Loc,
                            BaseReference,
                            TypeCharacter,
                            (ExpressionList *)NULL,
                            false,
                            ExprNoFlags | (Flags & ExprCreateColInitElement), // Microsoft: why was this ExprNoFlags rather than Flags? at least for CollectionInitializers we have to propagate that flag.
                            NULL),
                        ExprAccessDefaultProperty | ExprPropagatePropertyReference | ExprIsExplicitCallTarget,
                        NULL);

                BaseReferenceCalledWithoutArguments = true;
            }
        }
    }

    // If it is an unbound lambda, bind the lambda now, so we will infer a proper lambda.
    if (!IsBad(BaseReference) && BaseReference->bilop == SX_UNBOUND_LAMBDA)
    {
        Type* DelegateType = InferLambdaType(&BaseReference->AsUnboundLambdaExpression(), BaseReference->Loc);

        if(DelegateType)
        {
            BaseReference = ConvertWithErrorChecking(BaseReference, DelegateType, ExprNoFlags);
        }
    }

    // If the base reference is bad, treat the expression as a function
    // call. This will get the arguments interpreted.

    if
    (
        IsBad(BaseReference) ||
        (
            BaseReference->bilop == SX_SYM &&
            IsProcedure(BaseReference->AsSymbolReferenceExpression().Symbol)
        ) ||
        TypeHelpers::IsDelegateType(BaseReference->ResultType) ||
        BaseReference->bilop == SX_OVERLOADED_GENERIC ||
        BaseReference->bilop == SX_EXTENSION_CALL
    )
    {
        Result =
            BindArgsAndInterpretCallExpressionWithNoCopyOut(
                CallOrIndex->TextSpan,
                BaseReference,
                TypeCharacter,
                CallOrIndex->Arguments.Values,
                Flags,
                CallOrIndex->AlreadyResolvedTarget ? OvrldSkipTargetResolution : OvrldNoFlags,
                NULL);
    }

    else if (IsPropertyReference(BaseReference) || IsLateReference(BaseReference))
    {
        VSASSERT(
            BaseReference->AsPropertyReferenceExpression().Right == NULL,
            "Late-bound or property reference with spurious arguments.");

        if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                CallOrIndex->TextSpan);

            return AllocateBadExpression(CallOrIndex->TextSpan);
        }

        bool ArgumentsBad = false;

        ExpressionList *Arguments =
            InterpretArgumentList(
                CallOrIndex->Arguments.Values,
                ArgumentsBad,
                ExprNoFlags);

        if (ArgumentsBad)
        {
            MakeBad(BaseReference);
        }

        // Port SP1 CL 2922610 to VS10
        // for late bound calls we need to bind lambdas here
        if (IsLateReference(BaseReference))
        {
            for (ExpressionList *ArgumentList = Arguments;
                 ArgumentList;
                 ArgumentList = ArgumentList->AsExpressionWithChildren().Right)
            {
                VSASSERT(
                ArgumentList->bilop == SX_LIST,
               "Argument list must have SX_LIST type.");

                if (ArgumentList->bilop != SX_LIST)
                {
                    break;
                }

                ILTree::Expression * ArgNode = ArgumentList->AsExpressionWithChildren().Left;

                VSASSERT(
                (ArgNode == NULL) || (ArgNode->bilop == SX_ARG),
               "Argument must have SX_ARG type.");

                if (ArgNode && (ArgNode->bilop == SX_ARG))
                {
                    ILTree::Expression * ArgumentExpr = ArgNode->AsExpressionWithChildren().Left;

                    if (ArgumentExpr && (ArgumentExpr->bilop == SX_UNBOUND_LAMBDA))
                    {
                        ArgNode->AsExpressionWithChildren().Left =
                               ConvertWithErrorChecking(
                                    ArgumentExpr,
                                    GetFXSymbolProvider()->GetObjectType(),
                                    ExprNoFlags);
                    }
                }
            }
        }


        BaseReference->AsPropertyReferenceExpression().Right = Arguments;
        Result = BaseReference;
    }

    else if (TypeHelpers::IsArrayType(BaseReference->ResultType))
    {
        if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                CallOrIndex->TextSpan);

            return AllocateBadExpression(CallOrIndex->TextSpan);
        }

        ILTree::Expression *ArrayRef = MakeRValue(BaseReference, NULL);  // Dev10#480591: MakeRValue in case it was an array literal, to reclassify it to just an array

        // #539542 Check if reclassification failed, do not continue, an error should have been reported already.
        if (IsBad(ArrayRef))
        {
            return AllocateBadExpression(CallOrIndex->TextSpan);
        }

        Result =
            InterpretArrayIndexReference(
                CallOrIndex->TextSpan,
                ArrayRef,
                CallOrIndex->Arguments);
    }
    // both System.Object and System.Array can be indexed at runtime with a LateIndexGet call
    // 


    else if (TypeHelpers::IsRootObjectType(BaseReference->ResultType) ||
             TypeHelpers::IsRootArrayType(BaseReference->ResultType, m_CompilerHost))
    {
        if (HasFlag(Flags, ExprMustBeConstant))
        {
            ReportSemanticError(
                ERRID_RequiredConstExpr,
                CallOrIndex->TextSpan);

            return AllocateBadExpression(CallOrIndex->TextSpan);
        }


        if (BaseReference->bilop == SX_NOTHING)
        {
            ReportSemanticError(
                ERRID_IllegalCallOrIndex,
                CallOrIndex->TextSpan);

            return AllocateBadExpression(CallOrIndex->TextSpan);
        }

        // Option Strict disallows late binding.
        if (m_UsingOptionTypeStrict)
        {
            ReportSemanticError(ERRID_StrictDisallowsLateBinding, CallOrIndex->TextSpan);
            return AllocateBadExpression(CallOrIndex->TextSpan);
        }

        // Starlite Library doesn't support late binding, lower precedence than Option Strict
        if (m_CompilerHost->IsStarliteHost())
        {
            ReportSemanticError(
                ERRID_StarliteDisallowsLateBinding,
                CallOrIndex->TextSpan);

            return AllocateBadExpression(CallOrIndex->TextSpan);
        }

        // Warnings have lower precedence than Option Strict and Starlite errors.
        if (WarnOptionStrict())
        {
            ReportSemanticError(WRNID_LateBindingResolution, CallOrIndex->TextSpan);
        }

        Result =
            InterpretObjectIndexReference(
                CallOrIndex->TextSpan,
                BaseReference,
                CallOrIndex->Arguments.Values);
    }
    else
    {
        if (BaseReferenceCalledWithoutArguments)
        {
            ReportSemanticError(
                ERRID_FunctionResultCannotBeIndexed1,
                BaseReference->Loc,
                BaseReference);
        }
        else
        {
            ReportSemanticError(
                ERRID_IndexedNotArrayOrProc,
                BaseReference->Loc);
        }

        MakeBad(BaseReference);

        Result =
            BindArgsAndInterpretCallExpressionWithNoCopyOut(
                CallOrIndex->TextSpan,
                BaseReference,
                TypeCharacter,
                CallOrIndex->Arguments.Values,
                Flags,
                CallOrIndex->AlreadyResolvedTarget ? OvrldSkipTargetResolution : OvrldNoFlags,
                NULL);
    }

    if ((HasFlag(Flags, ExprIsExplicitCallTarget) ||
         !HasFlag(Flags, ExprPropagatePropertyReference)) &&
        !IsBad(Result) &&
        IsPropertyReference(Result))
    {
        // If this property or late-bound indexing is the operand of another
        // indexing, allowing this property or late reference to propagate leads
        // to confusion in processing the parent indexing. Therefore, interpret
        // this one.
        //
        // Also interpret the late/property reference in any context that does
        // not expect property propagation.

        Result = FetchFromProperty(Result);
    }

    return Result;
}

/******************************************************************************
;GetCanonicalTypeFromLocalCopy

Given an embedded local type, go find the canonical type. We know that the
canonical type must have the same fully qualified name, and we know that the
name-lookup code resolves overloads in favor of the non-embedded type, so all
we need to do is get the local type's fully qualified name and look it up.
If the type we find is not an embedded local type, and has the same identity
values as the local type we started with, then it is the canonical type and we
will return it in place of the local type. If we do not find a qualifying
canonical type, we will simply return the embedded local type, which must
stand for itself, the outFoundCanonicalType is set to False in that case.
******************************************************************************/
Type *
Semantics::GetCanonicalTypeFromLocalCopy
(
 Type *pType,
 bool & outFoundCanonicalType
 )
{
    outFoundCanonicalType = true;
    
    AssertIfNull(pType);
    AssertIfFalse(TypeHelpers::IsEmbeddedLocalType(pType));
    if (pType == NULL)
    {
        return pType;
    }

    Compiler *pCompiler = pType->PNamedRoot()->GetCompiler();
    AssertIfNull(pCompiler);

    CompilerProject *pCompilerProject = pCompiler->GetProjectBeingCompiled();
    CompilerHost *pCompilerHost = pType->PNamedRoot()->GetCompilerHost();
    AssertIfNull(pCompilerHost);
    
    BCSYM_Namespace *Scope = pCompilerProject ?
        pCompiler->GetUnnamedNamespace(pCompilerProject) :
        pCompiler->GetUnnamedNamespace();
    
    Location loc;
    if (pType->HasLocation())
    {
        loc = *(pType->GetLocation());
    }
    else
    {
        loc.SetLocationToHidden();
    }
    
    STRING *gqn = pType->PNamedRoot()->GetQualifiedName();
    unsigned NameCount = pCompiler->CountQualifiedNames(gqn);
    NorlsAllocator Scratch(NORLSLOC);
    STRING **Names = (STRING **)Scratch.Alloc(VBMath::Multiply(sizeof(STRING *),NameCount));
    bool NameIsBad = false;
    pCompiler->SplitQualifiedName(gqn, NameCount, Names);

    CompilationCaches *pCompilationCaches = GetCompilerCompilationCaches();

    Type *canonicalType =
        Semantics::InterpretQualifiedName(
            Names,
            NameCount,
            NULL,
            NULL,
            Scope->GetHash(),
            NameSearchIgnoreImports | NameSearchIgnoreModule | NameSearchCanonicalInteropType,
            loc,
            NULL,
            pCompiler,
            pCompilerHost,
            pCompilationCaches,
            NULL,
            true,
            NameIsBad,
            false,
            NULL,
            NULL,
            pType);

    // If we have located a type with the same fully qualified name, and it is semantically
    // equivalent to the embedded local type, but is not, itself, a local type, we will call 
    // it the canonical type and return it. Otherwise, we have failed to locate the canonical
    // type, so we will stick with the original (there may be multiple embedded local types
    // with the same fully-qualified name).
    if (canonicalType && !NameIsBad && 
        !TypeHelpers::IsEmbeddedLocalType(
            canonicalType
#if IDE 
            , true // fIgnoreTemporaryEmbeddedStatus
#endif                            
            ) &&
        (pType == canonicalType || TypeHelpers::AreTypeIdentitiesEquivalent(pType, canonicalType)))
    {
        return canonicalType;
    }
    else
    {
        outFoundCanonicalType = false;
        return pType;
    }
}

ILTree::Expression *
Semantics::InterpretIIF
(
    ParseTree::IIfExpression *ift,
    ExpressionFlags Flags
)
{
    // Figure out the general shape of the IF expression
    // This code uses "Arg" to refer to the parse-tree argument list, and "Operand" to refer to the
    // interpreted bound expression trees.
    int ArgCount=0;
    bool HasNamedArguments = false;
    for (ParseTree::ArgumentList *Arg = ift->Arguments.Values; Arg!=NULL; Arg=Arg->Next)
    {
        HasNamedArguments |= (Arg->Element->Name.Name != NULL);
        ArgCount++;
    }

    // Interpret the operands. We still interpret them even if there were the wrong number of arguments,
    // just so as to be able to produce a nicer output SX_tree in case of error.
    ParseTree::ArgumentList *Arg1 = ift->Arguments.Values;
    ParseTree::ArgumentList *Arg2 = (Arg1 != NULL) ? Arg1->Next : NULL;
    ParseTree::ArgumentList *Arg3 = (Arg2 != NULL) ? Arg2->Next : NULL;
    ParseTree::Expression *ArgExp1 = (Arg1!=NULL && Arg1->Element!=NULL) ? Arg1->Element->Value : NULL;
    ParseTree::Expression *ArgExp2 = (Arg2!=NULL && Arg2->Element!=NULL) ? Arg2->Element->Value : NULL;
    ParseTree::Expression *ArgExp3 = (Arg3!=NULL && Arg3->Element!=NULL) ? Arg3->Element->Value : NULL;
    ExpressionFlags IIFflags = (Flags & ExprMustBeConstant) | ExprDontInferResultType;
    // ExprMustBeConstant is the only flag we can inherit. And ExprDontInferType is so that
    // lambdas remain unbound; they, along with other typeless expressions (e.g. Nothing, AddressOf,
    // array literals) are handled in CreateCoalesceIIF and CreateTernaryIIF, as part of type inference.
    // 
    // Note: if we had let the lambda be interpreted now, then consider
    //    Dim x = function() 1
    //    Dim y = function() 1.2
    //    y = x    ' This involves delegate relaxation and an extra closure
    // Likewise, IF(function() 1, function() 1.2) would transform to
    //    Dim $temp1 As AnonDelegate(Of Int) = function() 1
    //    <expr> = if ($temp1 isnot nothing) then CType($temp1, AnonDelegate(Of Double)) else function() 1.2
    // That CType there would be inelegant. We avoid it by leaving the lambda as unbound for now,
    // and binding it later on.
    //
    ILTree::Expression *Operand1 = (ArgExp1==NULL) ? NULL :
                                        ((ArgCount>=3) ? InterpretConditionalOperand(ArgExp1, IIFflags)
                                                       : InterpretExpression(ArgExp1, IIFflags));
    ILTree::Expression *Operand2 = (ArgExp2==NULL) ? NULL : InterpretExpression(ArgExp2, IIFflags);
    ILTree::Expression *Operand3 = (ArgExp3==NULL) ? NULL : InterpretExpression(ArgExp3, IIFflags);

    // First we'll look for "structural" errors, i.e. errors that make it impossible for us to proceed
    // with useful interpretation, e.g. wrong number of arguments/operands, or named operands.
    bool StructuralError = false;

    if ((Operand1!=NULL && IsBad(Operand1)) ||
        (Operand2!=NULL && IsBad(Operand2)) ||
        (Operand3!=NULL && IsBad(Operand3)))
    {
        // bad operands will already have been reported
        StructuralError = true;
    }

    else if ((ArgCount!=2 && ArgCount!=3) ||
             (ArgCount==2 && (Operand1==NULL || Operand2==NULL)) ||
             (ArgCount==3 && (Operand1==NULL || Operand2==NULL || Operand3==NULL)))
    {
        // "'If' operator requires either two or three operands."
        ReportSemanticError(ERRID_IllegalOperandInIIFCount, ift->TextSpan);
        StructuralError = true;
    }

    else if (HasNamedArguments)
    {
        // "'If' operands cannot be named arguments."
        ReportSemanticError(ERRID_IllegalOperandInIIFName, ift->TextSpan);
        StructuralError = true;
    }

    // In case of structural errors we return a bad expression (for benefit of the IDE)
    if (StructuralError && ArgCount<=2)
    {
        return MakeBad(AllocateExpression(SX_IIFCoalesce,
                                          TypeHelpers::GetVoidType(),
                                          Operand1 ? Operand1 : AllocateBadExpression(ift->TextSpan),
                                          Operand2 ? Operand2 : AllocateBadExpression(ift->TextSpan),
                                          ift->TextSpan));
    }
    else if (StructuralError)
    {
        VSASSERT(ArgCount>=3, "internal logic error: ArgCount should be >=3 down this branch");
        return MakeBad(AllocateIIfExpression(//SX_IIF,
                                             TypeHelpers::GetVoidType(),
                                             Operand1 ? Operand1 : AllocateBadExpression(ift->TextSpan),
                                             Operand2 ? Operand2 : AllocateBadExpression(ift->TextSpan),
                                             Operand3 ? Operand3 : AllocateBadExpression(ift->TextSpan),
                                             ift->TextSpan));
    }


    // Otherwise, we'll call the appropriate function to do the rest of the work.
    // These functions check the types of their arguments, report errors if necessary,
    // and produce the final IF operator. Note that type-checking and inference is
    // very different between binary and ternary IF. For instance, IF(int?,int)
    // produces the dominant type between "int" and "int", while IF(bool,int?,int) produces
    // the dominant type between "int?" and "int".
    if (ArgCount==2)
    {
        VSASSERT(Operand1!=NULL && Operand2!=NULL && !IsBad(Operand1) && !IsBad(Operand2), "Internal logic error: binary if should have two fine operands here");
        return CreateCoalesceIIF(Operand1, Operand2, ift->TextSpan, IIFflags);
    }
    else if (ArgCount==3)
    {
        VSASSERT(Operand1!=NULL && Operand2!=NULL && Operand3!=NULL && !IsBad(Operand1) && !IsBad(Operand2) && !IsBad(Operand3), "Internal logic error: ternary if should have three fine operands here");
        return CreateTernaryIIF(Operand1, Operand2, Operand3, ift->TextSpan, IIFflags);
    }
    else
    {
        VSFAIL("Internal logic error: ArgCount should have been 2 or 3 by here");
        ReportSemanticError(ERRID_InternalCompilerError, ift->TextSpan);
        return AllocateBadExpression(ift->TextSpan);
    }
}


ILTree::Expression*
Semantics::CreateCoalesceIIF
(
    ILTree::Expression* Operand1,
    ILTree::Expression* Operand2,
    const Location &Loc,
    ExpressionFlags Flags
)
{
    if (Operand1==NULL || Operand2==NULL || IsBad(Operand1) || IsBad(Operand2))
    {
        VSFAIL("CreateCoalesceIIF expects better operands");
        ReportSemanticError(ERRID_InternalCompilerError, Loc);
        return AllocateBadExpression(Loc);
    }

    // CoalesceIIF, written IF(X,Y), roughly equivalent to IF(X ISNOT Nothing, X, Y)
    // X must be a nullable or a reference type.
    // If X is null, then it returns Y. Otherwise it returns X.
    // The transformation to IF(X ISNOT Nothing,...) is done in the lowering phase.
    // For now, we just save the expression as SX_IIFCOALESCE(x,y).
    //
    // As for the inferred result type, there's an interesting case e.g. IF(int?,int)
    // Operand1 is only used if it is non-null. Therefore the result type of this IF operator is simply "int".

    // Incidentally, if the types don't allow us to infer something for the ResultType, then
    // we'll mark the whole IF expression as bad and report an error.
    bool MarkResultAsBad = false;

    // For inference we use "InferDominantTypeOfExpressions". That does the right thing for typeless
    // expressions like Nothing, AddressOf, lambdas and array literals.
    ILTree::Expression *DominantWinner = NULL;

    // For the ResultType, we'll infer it...
    Type *ResultType = NULL;
    unsigned NumCandidates = 0;
    
    if (IsNothingLiteral(Operand1) && IsNothingLiteral(Operand2))
    {
        // Special case: IF(Nothing,Nothing) yields type Object
        ResultType = Operand2->ResultType;
        NumCandidates = 1;
    }
    else if (TypeHelpers::IsNullableType(Operand1->ResultType) && IsNothingLiteral(Operand2))
    {
        // Special case: IF(Nullable<T>,Nothing) yields type Nullable<T>.
        // (contrast with IF(Nullable<Int>,5) which yields 5.)
        ResultType = Operand1->ResultType;
        NumCandidates = 1;
    }
    else if (TypeHelpers::IsNullableType(Operand1->ResultType) && !TypeHelpers::IsNullableType(Operand2->ResultType))
    {
        // Special case: "nullable lifting"
        Type *Operand1RootType = TypeHelpers::GetElementTypeOfNullable(Operand1->ResultType, GetCompilerHost());
        ILTree::Expression *Operand1DummyExpression = AllocateExpression(SX_CTYPE, Operand1RootType, Loc);
        ResultType = InferDominantTypeOfExpressions(NumCandidates, DominantWinner, Operand1DummyExpression, Operand2);
    }
    else
    {
        // Normal case: dominant type
        ResultType = InferDominantTypeOfExpressions(NumCandidates, DominantWinner, Operand1, Operand2);

        // NB. the special case IF(Nothing, Value) is dealt with below. It has ResultType=Value,
        // as will be calculated here, but it needs Nothing to be converted to Value?
    }

    bool strict = m_UsingOptionTypeStrict;
    bool custom = !strict && WarnOptionStrict();

    if (ResultType->IsVoidType())
    {
        // "Cannot infer a common type."
        ReportSemanticError(ERRID_IfNoType, Loc);
        MarkResultAsBad = true;
    }
    else if (NumCandidates==0 && strict)
    {
        // "Cannot infer a common type, and Option Strict On does not allow 'Object' to be assumed."
        ReportSemanticError(ERRID_IfNoTypeObjectDisallowed, Loc);
        MarkResultAsBad = true;
    }
    else if (NumCandidates==0 && custom)
    {
        // "Cannot infer a common type; 'Object' assumed."
        StringBuffer buf;
        ReportSemanticError(WRNID_ObjectAssumed1, Loc, ResLoadString(WRNID_IfNoTypeObjectAssumed, &buf));
    }
    else if (NumCandidates>1 && strict)
    {
        // "Cannot infer a common type because more than one type is possible."
        ReportSemanticError(ERRID_IfTooManyTypesObjectDisallowed, Loc);
        MarkResultAsBad = true;
    }
    else if (NumCandidates>1 && custom)
    {
        // "Cannot infer a common type because more than one type is possible; 'Object' assumed."
        StringBuffer buf;
        ReportSemanticError(WRNID_ObjectAssumed1, Loc, ResLoadString(WRNID_IfTooManyTypesObjectAssumed, &buf));
    }

    // Const verification. The only posible const expr. here is IF(Nothing,ConstExp) or IF("some_string", ConstExp)
    if (!MarkResultAsBad && IsConstant(Operand1) && IsConstant(Operand2) &&
        (IsNothingLiteral(Operand1) || Operand1->bilop==SX_CNS_STR) &&
        (!m_IsGeneratingXML || HasFlag(Flags, ExprMustBeConstant)))
    {
        ILTree::Expression *Result = IsNothingLiteral(Operand1) ? Operand2 : Operand1;
        IDE_CODE(Result->uFlags |= (Operand1->uFlags | Operand2->uFlags ) & SXF_CON_CONTAINS_NAMED_CONTANTS);
        Result->Loc = Loc;
        return Result;
    }

    if (!MarkResultAsBad && HasFlag(Flags, ExprMustBeConstant))
    {
        ReportSemanticError(ERRID_RequiredConstExpr, Loc);
        return MakeBad(AllocateBadExpression(Loc));
    }


    // The operands need to be converted to ResultType. In the current codebase, that's partly 
    // left to Semantics::LoadIIfElementsFromIIfCoalesce later on. It deals with tricky things like
    // nullables in Operand1. But we're not allowed to exit this function leaving the tree still
    // with typeless elements in it. What we'll do as a compromise for now is to do conversions
    // on those typeless values (Nothing, AddressOf, ArrayLiterals, UnboundLambda, CallSub) to target type, but
    // nothing else.
    // Dev10#489077: the above list has to include lambdas as well!
    // Dev10#489150: and calls to subs!
    // Microsoft: consider: really, what this function should be doing is MakeRValue on all of its arguments.
    // It's a (bad) historical accident that MakeRValue fails to make rvalues out of SX_ADDRESSOF/SX_UNBOUND_LAMBDA.
    // That only happens through ConvertWithErrorChecking (which is called by ConvertExpressionToDominantType).
    if (!MarkResultAsBad)
    {
        if (Operand1->bilop==SX_ADDRESSOF || Operand1->bilop==SX_UNBOUND_LAMBDA || Operand1->bilop==SX_NOTHING ||
            (Operand1->bilop==SX_CALL && Operand1->ResultType->IsVoidType()) ||
            (Operand1->bilop==SX_ARRAYLITERAL && Operand1->ResultType->IsArrayLiteralType()))
        {
            // special case: IF(Nothing, Value) interprets the Nothing as Value?, so long as Value isn't Nullable
            if (Operand1->bilop==SX_NOTHING && TypeHelpers::IsValueType(ResultType) && !TypeHelpers::IsNullableType(ResultType))
            {
                if (!GetFXSymbolProvider()->IsTypeAvailable(FX::GenericNullableType))
                {
                    ReportMissingType(FX::GenericNullableType, Loc);
                    return MakeBad(AllocateBadExpression(Loc));
                }

                BCSYM *NullableResultType = GetFXSymbolProvider()->GetNullableType(ResultType, &m_SymbolCreator);
                Operand1 = ConvertWithErrorChecking(Operand1, NullableResultType, ExprNoFlags);
            }
            else
            {
                Operand1 = ConvertExpressionToDominantType(Operand1, ResultType, DominantWinner, ExprNoFlags);
            }
        }
        if (Operand2->bilop==SX_ADDRESSOF || Operand2->bilop==SX_UNBOUND_LAMBDA || Operand2->bilop==SX_NOTHING ||
            (Operand2->bilop==SX_CALL && Operand2->ResultType->IsVoidType()) ||
            (Operand2->bilop==SX_ARRAYLITERAL && Operand2->ResultType->IsArrayLiteralType()))
        {
            Operand2 = ConvertExpressionToDominantType(Operand2, ResultType, DominantWinner, ExprNoFlags);
        }

        if (IsBad(Operand1) || IsBad(Operand2))
        {
            // Dev10#530038: the call to "ConvertExpressionToDominantType" or "ConvertWithErrorChecking"
            // might have produced errors:
            MarkResultAsBad = true;
        }
    }

    // Note: this test comes after Operand1 has been converted.
    if (!MarkResultAsBad &&
        !TypeHelpers::IsReferenceType(Operand1->ResultType) &&
        !TypeHelpers::IsNullableType(Operand1->ResultType, GetCompilerHost()))
    {
        // "First operand in a binary 'If' expression must be nullable or a reference type."
        ReportSemanticError(ERRID_IllegalCondTypeInIIF, Operand1->Loc);
        Operand1 = MakeBad(Operand1);
        MarkResultAsBad = true;
    }

    // This allocated expression will work whether MarkResultAsBad or not.
    ILTree::Expression *Result = AllocateExpression(SX_IIFCoalesce, ResultType, Operand1, Operand2, Loc);
    Result = MarkResultAsBad ? MakeBad(Result) : Result;
    return Result;
}


ILTree::Expression*
Semantics::CreateTernaryIIF
(
    ILTree::Expression* Condition,
    ILTree::Expression* ThenExpression,
    ILTree::Expression* ElseExpression,
    const Location &Loc,
    ExpressionFlags Flags
)
{
    if (Condition==NULL || ThenExpression==NULL || ElseExpression==NULL ||
        IsBad(Condition) || !IsBooleanType(Condition->ResultType->GetVtype()) || IsBad(ThenExpression) || IsBad(ElseExpression))
    {
        VSFAIL("CreateTenraryIIF expects better operands");
        ReportSemanticError(ERRID_InternalCompilerError, Loc);
        return AllocateBadExpression(Loc);
    }
    
    // TernaryIIF, written IF(Condition,ThenExpression,ElseExpression)
    // Condition is assumed already to be of boolean type.


    // Incidentally, if the types don't allow us to infer something for the ResultType, then
    // we'll mark the whole IF expression as bad and report an error.
    bool MarkResultAsBad = false;

    // For inference we use "InferDominantTypeOfExpressions". That does the right thing for typeless
    // expressions like Nothing, AddressOf, lambdas and array literals.
    ILTree::Expression *DominantWinner = NULL;

    // And here's where we infer the result type:
    Type *ResultType = NULL;
    unsigned NumCandidates = 0;
    
    if (IsNothingLiteral(ThenExpression) && IsNothingLiteral(ElseExpression))
    {
        // backwards compatibility with Orcas... IF(b,Nothing,Nothing) infers Object with no complaint
        ResultType = ThenExpression->ResultType;
        NumCandidates = 1;
    }
    else
    {
        ResultType = InferDominantTypeOfExpressions(NumCandidates, DominantWinner, ThenExpression, ElseExpression);
    }

    bool strict = m_UsingOptionTypeStrict;
    bool custom = !strict && WarnOptionStrict();

    if (ResultType->IsVoidType())
    {
        // "Cannot infer a common type."
        ReportSemanticError(ERRID_IfNoType, Loc);
        MarkResultAsBad = true;
    }
    else if (NumCandidates==0 && strict)
    {
        // "Cannot infer a common type, and Option Strict On does not allow 'Object' to be assumed."
        ReportSemanticError(ERRID_IfNoTypeObjectDisallowed, Loc);
        MarkResultAsBad = true;
    }
    else if (NumCandidates==0 && custom)
    {
        // "Cannot infer a common type; 'Object' assumed."
        StringBuffer buf;
        ReportSemanticError(WRNID_ObjectAssumed1, Loc, ResLoadString(WRNID_IfNoTypeObjectAssumed, &buf));
    }
    else if (NumCandidates>1 && strict)
    {
        // "Cannot infer a common type because more than one type is possible."
        ReportSemanticError(ERRID_IfTooManyTypesObjectDisallowed, Loc);
        MarkResultAsBad = true;
    }
    else if (NumCandidates>1 && custom)
    {
        // "Cannot infer a common type because more than one type is possible; 'Object' assumed."
        StringBuffer buf;
        ReportSemanticError(WRNID_ObjectAssumed1, Loc, ResLoadString(WRNID_IfTooManyTypesObjectAssumed, &buf));
    }

    // The operands need to be converted to ResultType. Note that this has the potential
    // to turn NothingLiterals into e.g. values, which could be a problem for our "Const verification" below.
    // But actually, const verification only needs to catch the case where both ThenExpression
    // and ElseExpression were both objects, in which case their dominant type will be Object,
    // and they'll both be left as SX_NOTHINGs so that's fine.
    if (!MarkResultAsBad)
    {
        ThenExpression = ConvertExpressionToDominantType(ThenExpression, ResultType, DominantWinner, Flags | ExprSuppressWideCoerce);
        ElseExpression = ConvertExpressionToDominantType(ElseExpression, ResultType, DominantWinner, Flags | ExprSuppressWideCoerce);

        if (IsBad(ThenExpression) || IsBad(ElseExpression))
        {
            // Dev10#530038: the call to "ConvertExpressionToDominantType" or "ConvertWithErrorChecking"
            // might have produced errors:
            MarkResultAsBad = true;
        }
    }


    // Const verification.
    if (!MarkResultAsBad && IsConstant(Condition) && IsConstant(ThenExpression) && IsConstant(ElseExpression) &&
        (!m_IsGeneratingXML || HasFlag(Flags, ExprMustBeConstant)) &&
        ( (IsNothingLiteral(ThenExpression) && IsNothingLiteral(ElseExpression))
          ||
          (AllowsCompileTimeOperations(ResultType) &&
           AllowsCompileTimeOperations(Condition->ResultType) &&
           AllowsCompileTimeOperations(ThenExpression->ResultType) &&
           AllowsCompileTimeOperations(ElseExpression->ResultType))))
    {
        Quadword Value = Condition->AsIntegralConstantExpression().Value;
        ILTree::Expression *Result = Value ? ThenExpression : ElseExpression;
        IDE_CODE(Result->uFlags |= (Condition->uFlags | ThenExpression->uFlags | ElseExpression->uFlags) & SXF_CON_CONTAINS_NAMED_CONTANTS);
        Result->Loc = Loc;
        return Result;
    }

    if (!MarkResultAsBad && HasFlag(Flags, ExprMustBeConstant))
    {
        ReportSemanticError(ERRID_RequiredConstExpr, Loc);
        return MakeBad(AllocateBadExpression(Loc));
    }

    // And this is the final result.
    ILTree::Expression *Result = AllocateIIfExpression(ResultType, Condition, ThenExpression, ElseExpression, Loc);
    Result = MarkResultAsBad ? MakeBad(Result) : Result;
    return Result;
}


ILTree::Expression*
Semantics::ForceLiftToEmptyString
(
    ILTree::Expression *pExpr,
    Type    *ResultType
)
{
    AssertIfFalse(TypeHelpers::IsStringType(ResultType));
    //Expression *NullStr =
    //    CreateConstructedInstance(
    //        ResultType,
    //        pExpr->Loc,
    //        pExpr->Loc,
    //        AllocateExpression(
    //            SX_LIST,
    //            TypeHelpers::GetVoidType(),
    //            AllocateExpression(
    //                SX_ARG,
    //                TypeHelpers::GetVoidType(),
    //                AllocateExpression(
    //                    SX_NOTHING,
    //                    GetFXSymbolProvider()->GetObjectType(),
    //                    pExpr->Loc),
    //                pExpr->Loc),
    //            pExpr->Loc),
    //        false,
    //        ExprNoFlags);

    ILTree::Expression *NullStr = ProduceStringConstantExpression(NULL, 0, pExpr->Loc IDE_ARG(0));

    ILTree::Expression *Result = AllocateExpression(
                SX_IIFCoalesce,
                ResultType,
                pExpr,
                NullStr,
                pExpr->Loc);

    Result->ForcedLiftedCatenationIIFCoalesce = true;
    return Result;

}


Semantics::ExpressionListHelper::ExpressionListHelper
(
    Semantics * pSemantics,
    ExpressionList * pList
) :
    m_ppListEnd(NULL),
    m_count(0),
    m_pSemantics(pSemantics),
    m_pListStart(NULL)
{
    Init(pList);
}

void Semantics::ExpressionListHelper::Init
(
    ExpressionList * pList
)
{
    m_pListStart = pList;
    if (pList)
    {
        m_ppListEnd = &(pList->AsExpressionWithChildren().Right);
        m_count = 1;
    }
    else
    {
        m_count = 0;
        m_ppListEnd = &(m_pListStart);
    }

    AdvanceToEnd();
}

void Semantics::ExpressionListHelper::Add(ILTree::Expression * pExpr, const Location & location)
{
    VSASSERT(m_ppListEnd, "ExpressionListHelper is in an invalid state.");

    if (m_ppListEnd)
    {
        ILTree::Expression * pNode =
            m_pSemantics->AllocateExpression
            (
                SX_LIST,
                TypeHelpers::GetVoidType(),
                pExpr,
                NULL,
                location
            );

        *m_ppListEnd = pNode;

        m_ppListEnd = &(pNode->AsExpressionWithChildren().Right);
        ++m_count;
    }
}

void Semantics::ExpressionListHelper::Splice(ExpressionList * pList)
{
    Assume(m_ppListEnd, L"ExpressionListHelper is in an invalid state. Did you call Init?");

    *m_ppListEnd = pList;
    AdvanceToEnd();
}

unsigned long Semantics::ExpressionListHelper::Count
(
)
{
    return m_count;
}

ILTree::ExpressionWithChildren* Semantics::ExpressionListHelper::Start()
{
    return m_pListStart ? &(m_pListStart->AsExpressionWithChildren()) : NULL;
}

void Semantics::ExpressionListHelper::AdvanceToEnd()
{
    while (m_ppListEnd && *m_ppListEnd)
    {
        m_ppListEnd = &((*m_ppListEnd)->AsExpressionWithChildren().Right);
        ++m_count;
    }
}

//If you are looking at a windiff trying to figure out what's going on
//with this file so that you can do an integration, here's the jist of it:
//
//InterpretGenericQualifiedExpression was renamed to
//InterpretGenericQualifiedSymbolExpression, and  then a new
//procedure named InterpretGenericQualifiedExpression was added.
//The new InterpretGenericQualifiedExpression contains contents that used to be under
//the Generic Qualified expression branch of InterpretExpression.
ILTree::Expression *
Semantics::InterpretGenericQualifiedSymbolExpression
(
    ParseTree::GenericQualifiedExpression * GenericQualified,
    ILTree::SymbolReferenceExpression * BaseReference,
    Type **BoundArguments,
    Location * TypeArgumentLocations,
    unsigned int ArgumentCount,
    ExpressionFlags Flags
)
{
    ILTree::Expression * Result = NULL;
    Declaration *Generic = BaseReference->Symbol;
    bool ResultIsBad = false;

    if
    (
        IsProcedure(Generic) &&
        (
            ViewAsProcedure(Generic)->IsOverloads()
        )
    )
    {
        // The generic arguments cannot be checked against the generic parameters
        // until after overload resolution.

        Result =
            AllocateExpression
            (
                SX_OVERLOADED_GENERIC,
                TypeHelpers::GetVoidType(),
                GenericQualified->TextSpan
            );

        Result->AsOverloadedGenericExpression().BaseReference = BaseReference;
        Result->AsOverloadedGenericExpression().TypeArguments = BoundArguments;
        Result->AsOverloadedGenericExpression().TypeArgumentCount = ArgumentCount;
        Result->AsOverloadedGenericExpression().TypeArgumentLocations = TypeArgumentLocations;

    }
    else
    {
        VSASSERT(ArgumentCount != 0, "generic qualified name parse trees messed up!!!");

        Bindable::ValidateArity(
            Generic->GetName(),
            Generic,
            NULL,
            ArgumentCount,
            &GenericQualified->Arguments.Arguments->TextSpan,
            m_ReportErrors ? m_Errors : NULL,
            m_Compiler,
            ResultIsBad);

        if (ResultIsBad)
        {
            return AllocateBadExpression(GenericQualified->TextSpan);
        }

        // Given a generic declaration, the binding used to refer to the generic, and the
        // set of type arguments, ValidateGenericArguments checks the arguments against the
        // constraints of the generic and returns a new binding (using the arguments) with
        // the binding used to refer to the generic as the parent binding.

        GenericBinding *Binding =
            ValidateGenericArguments(
                GenericQualified->TextSpan,
                Generic,
                BoundArguments,
                TypeArgumentLocations,
                ArgumentCount,
                BaseReference->AsSymbolReferenceExpression().GenericBindingContext->PGenericTypeBinding(),
                ResultIsBad);

        if (ResultIsBad)
        {
            return AllocateBadExpression(GenericQualified->TextSpan);
        }

        BaseReference->GenericBindingContext = Binding;

        // For generic types, the result type of the expression referring to them
        // should be the generic binding corresponding to the given arguments.
        //
        if (Binding && !IsProcedure(Generic))
        {
            BaseReference->ResultType = Binding;
        }

        BaseReference->Loc = GenericQualified->TextSpan;
        Result = BaseReference;
    }

    if (IsProcedure(Generic) && !(Flags & ExprIsExplicitCallTarget))
    {
        Result =
            InterpretCallExpressionWithNoCopyout(
                GenericQualified->TextSpan,
                Result,
                ExtractTypeCharacter(GenericQualified),
                (ExpressionList *)NULL,
                false,
                Flags,
                NULL);
    }

    return Result;
}


/*static*/
STRING *
Semantics::GenerateUniqueName
(
Compiler *compiler,
const WCHAR *rootName,
unsigned counter
)
{
    // 
    AssertIfNull(compiler);
    AssertIfNull(rootName);

    // Build a stub with the name _Closure_<counter>
    StringBuffer name;
    name.AppendPrintf(L"%s_%X",
            rootName,
            counter);

    return compiler->AddString(&name);
}

// ----------------------------------------------------------------------------
// Check if the type is a generic binding for an anonymous type.
// ----------------------------------------------------------------------------

bool
Semantics::IsAnonymousType
(
    Type *candidate
)
{
    ThrowIfNull( candidate );

    bool bRet = false;

    if( candidate->IsContainer() &&
        candidate->PContainer()->IsAnonymousType() ||    // Anonymous type with no field case will not be generic.
        candidate->IsGenericTypeBinding() &&
        candidate->PGenericTypeBinding()->GetGeneric()->IsContainer() &&
        candidate->PGenericTypeBinding()->GetGeneric()->PContainer()->IsAnonymousType() )
    {
        bRet = true;
    }

    return( bRet );
}

ILTree::Expression *
Semantics::ConvertLambdaToExpressionTree
(
    ILTree::Expression *Input,
    ExpressionFlags Flags,
    Type *TargetType
)
{
    ILTreeETGenerator ExprTreeGenerator(this);
    ExpressionTreeSemantics<ILTree::Expression> ExprTreeSemantics(this, &ExprTreeGenerator, &ExprTreeGenerator);

    return ExprTreeSemantics.ConvertLambdaToExpressionTree(
        Input,
        Flags,
        TargetType
        );
}

// ----------------------------------------------------------------------------
// Check whether we should convert this lhs/rhs pair to an expression tree.
// ----------------------------------------------------------------------------

bool
Semantics::IsConvertibleToExpressionTree
(
    Type* TargetType,
    ILTree::Expression* Input,
    ILTree::LambdaExpression **LambdaExpr
)
{
    ThrowIfNull( TargetType );
    ThrowIfNull( Input );

    bool bRet = false;

    // We only allow conversions if the LHS is an Expression(Of T) and the RHS
    // is a lambda expression.
    //
    // !!! HACK ALERT !!!
    // Ultimately, one day, we will allow multiline lambdas to be converted to expression-trees.
    // Imagine if the current overload semantics disallowed such conversions, but future overload semantics
    // did allow it. That would make the feature a backwards-compatibility-break. We didn't want
    // the break. Therefore, we say here and now that a multiline lambda IsConvertibleToExpressionTree,
    // but much later on (in closures.cpp, FixupExpressionTrees), we report a not-yet-implemented error.
    // There are further issues arising. Our plan for creating expression trees is (1) create the lambda
    // closures and fixup symbol references, (2) then turn them into expression-trees. To fixup symbol
    // references we need to know the parent of each symbol. Parents for multiline lambdas haven't yet
    // been implemented properly. Search for 







    if( m_CompilerHost->GetFXSymbolProvider()->IsTypeAvailable(FX::GenericExpressionType) &&
        IsLambdaExpressionTree( TargetType ) &&
        ( (Input->bilop == SX_LAMBDA) || (Input->bilop == SX_UNBOUND_LAMBDA))
      )
    {
        bRet = true;
        if ( LambdaExpr && SX_LAMBDA == Input->bilop )
        {
            *LambdaExpr = &Input->AsLambdaExpression();
        }
    }

    return( bRet );
}

// ----------------------------------------------------------------------------
// Check whether the target is a lambda expression.
// ----------------------------------------------------------------------------

bool
Semantics::IsLambdaExpressionTree
(
    Type* TargetType
)
{
    ThrowIfNull( TargetType );

    bool bRet = false;

    // Anonymous types need to be filtered out before we call IsOrInheritsFrom
    // because we don't bind bases in method bodies, and we know that anonymous
    // types never inherit from expression.

    if( m_CompilerHost->GetFXSymbolProvider()->IsTypeAvailable(FX::GenericExpressionType) &&
        ( !IsAnonymousType( TargetType ) &&
          IsOrInheritsFrom( TargetType, GetFXSymbolProvider()->GetGenericExpressionType() ) ) )
    {
        // 



        if (TargetType->IsGenericTypeBinding())
        {
            BCSYM *pGenericArg = TargetType->PGenericTypeBinding()->GetArgument(0);

            ThrowIfNull(pGenericArg);

            bRet =  TypeHelpers::IsDelegateType(pGenericArg) || TypeHelpers::IsGenericParameter(pGenericArg);
        }
        else
        {
            bRet = true;
        }
    }

    return( bRet );
}

// Gives us access to the /langVersion switch utilities - defined in langversion.cpp
extern struct LanguageFeatureMap g_LanguageFeatureMap[]; // The list of features and version they were introduced for use with /langVersion 
extern int g_CountOfPreviousLanguageVersions; // the number of elements in g_LanguageFeatureMap
extern WCHAR *g_LanguageVersionStrings[]; // strings for previous versions of vb; used in error reporting to indicate when a feature was introduced 


/*****************************************************************************************
;ReportSyntaxErrorForLanguageFeature

Reports errors for cases where a feature was introduced after the version specified by
the /LangVersion switch.  

The reason that this function is seperate from the normal error reporting functions is that
we don't want to mark the current statement as being in error because processing should
proceed normally - we simply want to flag those language constructs that aren't available
in versions of the compiler that aren't being targeted with /LangVersion.  
If we logged the statements as being in error, we could compromise the ability to get 
through the text this version of the compiler does understand and thus mess up our ability
to give good errors where we diverge from what an earlier version of the compiler understood.
******************************************************************************************/
void Semantics::ReportSyntaxErrorForLanguageFeature
(
    unsigned Errid, // the error to log.  
    _In_ const Location &ErrLocation, // The location for the error squiggles 
    unsigned Feature, // A FEATUREID_* constant defined in errors.inc
    _In_opt_z_ const WCHAR *wszVersion // the string for the version that /LangVersion is targeting
)
{
    if (m_ReportErrors)
    {
        WCHAR wszLoad[CCH_MAX_LOADSTRING];
        IfFailThrow( ResLoadString( Feature, wszLoad, DIM( wszLoad ) ) );

        ReportSemanticError( Errid, ErrLocation, wszLoad, wszVersion );
    }
}

/*****************************************************************************************
;AssertLanguageFeature

Given a FEATUREID_*, determines if that feature was introduced after the version that is
currently being targeted via the /LangVersion switch.  If it was introduced after the 
version being targeted, an error is raised to that effect.
******************************************************************************************/
void Semantics::AssertLanguageFeature(
    unsigned feature, // the feature we are testing for, e.g. FEATUREID_StatementLambdas
    _Inout_ Location const &ErrorLocation // if we end up logging an error, this token will be used for the error location
)
{
    // If we are targeting the latest version of the compiler, all features are fair game
    if ( m_CompilingLanguageVersion == LANGUAGE_CURRENT )
    {
        return;
    }

    // If a feature was introduced after the version of the compiler we are targeting
    if ( m_CompilingLanguageVersion < g_LanguageFeatureMap[ FEATUREID_TO_INDEX(feature) ].m_Introduced )
    {
        const WCHAR* wszVersion = NULL;
        if ( m_CompilingLanguageVersion < g_CountOfPreviousLanguageVersions )
        {
            wszVersion = g_LanguageVersionStrings[ m_CompilingLanguageVersion ];
        }
        else
        {
            wszVersion = L"???";
        }

        AssertIfNull( wszVersion );
        ReportSyntaxErrorForLanguageFeature(ERRID_LanguageVersion, ErrorLocation, feature, wszVersion );
    }
}

