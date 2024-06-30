#include "CSGenerator.h"
#include "CSModule.h"
#include "CSharpGeneratorUtilities.h"
#include "GlueGeneratorModule.h"
#include "CSScriptBuilder.h"
#include "CSPropertyTranslatorManager.h"
#include "PropertyTranslators/PropertyTranslator.h"
#include "PropertyTranslators/DelegateBasePropertyTranslator.h"
#include "GameFramework/FloatingPawnMovement.h"
#include "GameFramework/SpringArmComponent.h"
#include "Interfaces/IPluginManager.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/ScopedSlowTask.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "UnrealSharpUtilities/UnrealSharpStatics.h"
#include "Kismet/BlueprintAsyncActionBase.h"
#include "UObject/Field.h"

void FCSGenerator::StartGenerator(const FString& OutputDirectory)
{
	if (bInitialized)
	{
		return;
	}

	bInitialized = true;
	GeneratedScriptsDirectory = OutputDirectory;

	CheckGlueGeneratorVersion();

	//TODO: SUPPORT THESE BUT CURRENTLY TOO LAZY TO FIX
	{
		DenyList.AddClass("AnimationBlueprintLibrary");
		DenyList.AddFunctionCategory(UKismetMathLibrary::StaticClass()->GetFName(), "Math|Vector4");
	}

	AllowList.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, AddComponentByClass));
	
	BlueprintInternalAllowList.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, UserConstructionScript));
	BlueprintInternalAllowList.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, AddComponentByClass));
	BlueprintInternalAllowList.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, FinishAddComponent));
	BlueprintInternalAllowList.AddFunction(UBlueprintAsyncActionBase::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(UBlueprintAsyncActionBase, Activate));
	
	OverrideInternalList.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, AddComponentByClass));
	OverrideInternalList.AddFunction(AActor::StaticClass()->GetFName(), GET_FUNCTION_NAME_CHECKED(AActor, FinishAddComponent));

	PropertyTranslatorManager.Reset(new FCSPropertyTranslatorManager(NameMapper, DenyList));

	FModuleManager::Get().OnModulesChanged().AddRaw(this, &FCSGenerator::OnModulesChanged);

	// Get all currently loaded types that are in the engine
	TArray<UObject*> PackagesToProcess;
	GetObjectsOfClass(UPackage::StaticClass(), PackagesToProcess);
	for (UObject* ObjectToProcess : PackagesToProcess)
	{
		GenerateGlueForPackage(static_cast<UPackage*>(ObjectToProcess));
	}

	// Generate glue for some common types that don't get picked up.
	GenerateGlueForType(UInterface::StaticClass(), true);
	GenerateGlueForType(UObject::StaticClass(), true);
	GenerateGlueForType(USpringArmComponent::StaticClass(), true);
	GenerateGlueForType(UFloatingPawnMovement::StaticClass(), true);
}

void FCSGenerator::GenerateGlueForPackage(const UPackage* Package)
{
	TArray<UObject*> ObjectsToProcess;
	GetObjectsWithPackage(Package, ObjectsToProcess, false, RF_ClassDefaultObject);

	for (UObject* ObjectToProcess : ObjectsToProcess)
	{
		GenerateGlueForType(ObjectToProcess);
	}

	GenerateExtensionMethodsForPackage(Package);
}

void FCSGenerator::OnModulesChanged(FName InModuleName, EModuleChangeReason InModuleChangeReason)
{
	if (InModuleChangeReason != EModuleChangeReason::ModuleLoaded)
	{
		return;
	}
	
	const UPackage* ModulePackage = FindPackage(nullptr, *FString::Printf(TEXT("/Script/%s"), *InModuleName.ToString()));
	
	if (!ModulePackage)
	{
		return;
	}

	GenerateGlueForPackage(ModulePackage);
}

#define LOCTEXT_NAMESPACE "FScriptGenerator"

void FCSGenerator::GenerateGlueForTypes(TArray<UObject*>& ObjectsToProcess)
{
	FScopedSlowTask SlowTask(2, LOCTEXT("GeneratingGlue", "Processing C# bindings..."));
	SlowTask.EnterProgressFrame(1);
	
	for (UObject* ObjectToProcess : ObjectsToProcess)
	{
		GenerateGlueForType(ObjectToProcess);
	}
	
	GeneratedFileManager.RenameTempFiles();
	SlowTask.EnterProgressFrame(1);
}

void FCSGenerator::GenerateGlueForType(UObject* Object, bool bForceExport)
{
	// We don't want stuff in the transient package - that stuff is just temporary
	if (Object->GetOutermost() == GetTransientPackage())
	{
		return;
	}
	
	if (ExportedTypes.Contains(Object))
	{
		return;
	}

	FCSScriptBuilder Builder(FCSScriptBuilder::IndentType::Spaces);
	
	if (UClass* Class = Cast<UClass>(Object))
	{
		// Don't generate glue for templates
		if (Class->HasAnyFlags(RF_ClassDefaultObject))
		{
			return;
		}
		
		// Don't generate glue for classes that are generated from blueprints or C#
		if (Cast<UBlueprintGeneratedClass>(Class) || Class->HasAnyClassFlags(CLASS_CompiledFromBlueprint))
		{
			return;
		}

		// Don't generated glue for classes that have been regenerated in memory (this is the old version of the class)
		if (Class->HasAnyClassFlags(CLASS_NewerVersionExists))
		{
			return;
		}

		// Don't generate glue for TRASH_ classes. These are classes that have been deleted but are still in memory.
		if (Class->GetName().Find(TEXT("TRASH_")) != INDEX_NONE)
		{
			return;
		}

		// Don't generate glue for REINST_ classes. These are classes that have been recompiled but are still in memory, and will soon be TRASH_ classes.
		if (Class->GetName().Find(TEXT("REINST_")) != INDEX_NONE)
		{
			return;
		}

		if (Class->HasMetaData("NotGeneratorValid"))
		{
			return;
		}
		
		RegisterClassToModule(Class);
				
		if (Class->IsChildOf(UInterface::StaticClass()))
		{
			ExportInterface(Class, Builder);
		}
		else if (bForceExport || ShouldExportClass(Class))
		{
			ExportClass(Class, Builder);
		}
	}
	else if (UScriptStruct* Struct = Cast<UScriptStruct>(Object))
	{
		if (bForceExport || ShouldExportStruct(Struct))
		{
			ExportStruct(Struct, Builder);
		}
	}
	else if (UEnum* Enum = Cast<UEnum>(Object))
	{
		if (bForceExport || ShouldExportEnum(Enum))
		{
			ExportEnum(Enum, Builder);
		}
	}

	if (Builder.IsEmpty())
	{
		return;
	}
	
	SaveTypeGlue(Object->GetOutermost(), Object->GetName(), Builder);
}

void FCSGenerator::GenerateGlueForDelegate(UFunction* DelegateSignature, bool bForceExport)
{
	// We don't want stuff in the transient package - that stuff is just temporary
	if (DelegateSignature->GetOutermost() == GetTransientPackage())
	{
		return;
	}

	if (ExportedDelegates.Contains(DelegateSignature))
	{
		return;
	}

	FCSScriptBuilder Builder(FCSScriptBuilder::IndentType::Spaces);

	ExportDelegate(DelegateSignature, Builder);

	if (Builder.IsEmpty())
	{
		return;
	}

	FCSModule& Module = Get().FindOrRegisterModule(DelegateSignature->GetOutermost());
	FString DelegateName = FDelegateBasePropertyTranslator::GetDelegateName(DelegateSignature);

	FString FileName = FString::Printf(TEXT("%s.generated.cs"), *DelegateName);
	Get().SaveGlue(Module, FileName, Builder.ToString());
}

void FCSGenerator::GenerateExtensionMethodsForPackage(const UPackage* Package)
{
	FCSModule& Module = FindOrRegisterModule(Package);
	if (TArray<ExtensionMethod>* FoundExtensionMethods = ExtensionMethods.Find(Module.GetModuleName()))
	{
		FCSScriptBuilder Builder(FCSScriptBuilder::IndentType::Spaces);
		FString ClassName = FString::Printf(TEXT("%sExtensions"), *Module.GetModuleName().ToString());
		Builder.GenerateScriptSkeleton(Module.GetNamespace());
		Builder.DeclareType("static class", ClassName, "", false);
		
		for (const ExtensionMethod& Method : *FoundExtensionMethods)
		{
			PropertyTranslatorManager->Find(Method.Function).ExportExtensionMethod(Builder, Method);
		}

		Builder.CloseBrace();
		SaveTypeGlue(Package, ClassName, Builder);
	}
}

#undef LOCTEXT_NAMESPACE

bool FCSGenerator::CanExportClass(UClass* Class) const
{
	return !DenyList.HasClass(Class);
}

bool FCSGenerator::CanDeriveFromNativeClass(UClass* Class)
{
	const bool bCanCreate = !Class->HasAnyClassFlags(CLASS_Deprecated) && !Class->HasAnyClassFlags(CLASS_NewerVersionExists) && !Class->ClassGeneratedBy;
	
	const bool bIsBlueprintBase = FKismetEditorUtilities::CanCreateBlueprintOfClass(Class);
	
	const bool bIsValidClass = bIsBlueprintBase || AllowList.HasClass(Class) || Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass());

	return Class->IsChildOf(USubsystem::StaticClass()) || (bCanCreate && bIsValidClass);
}

void FCSGenerator::ExportEnum(UEnum* Enum, FCSScriptBuilder& Builder)
{
	const FCSModule& Module = FindOrRegisterModule(Enum);

	Builder.GenerateScriptSkeleton(Module.GetNamespace());

	AppendTooltip(Enum, Builder);
	Builder.AppendLine(TEXT("[UEnum]"));

	int64 MaxValue = Enum->GetMaxEnumValue();

	FString TypeDerived;
	if (MaxValue <= static_cast<int64>(UINT8_MAX))
	{
		TypeDerived = TEXT("byte");
	}
	else if (MaxValue <= static_cast<int64>(INT32_MAX))
	{
		TypeDerived = TEXT("int");
	}
	else if (MaxValue <= static_cast<int64>(UINT32_MAX))
	{
		TypeDerived = TEXT("uint");
	}
	else
	{
		TypeDerived = TEXT("long");
	}

	Builder.DeclareType("enum", *Enum->GetName(), *TypeDerived, false);
		
	FString CommonPrefix;

	const int32 ValueCount = Enum->NumEnums();
		
	for (int32 i = 0; i < ValueCount; ++i)
	{
		if (!ShouldExportEnumEntry(Enum, i))
		{
			continue;
		}

		FString RawName;

		FName ValueName = Enum->GetNameByIndex(i);
		int64 Value = Enum->GetValueByIndex(i);
		FString QualifiedValueName = ValueName.ToString();
		const int32 ColonPosition = QualifiedValueName.Find("::");

		if (ColonPosition != INDEX_NONE)
		{
			RawName = QualifiedValueName.Mid(ColonPosition + 2);
		}
		else
		{
			RawName = QualifiedValueName;
		}

		if (RawName.IsEmpty())
		{
			continue;
		}

		if (i == ValueCount - 1 && RawName.EndsWith("MAX"))
		{
			continue;
		}


		AppendTooltip(Enum->GetToolTipTextByIndex(i), Builder);
		Builder.AppendLine(FString::Printf(TEXT("%s=%lld,"), *RawName, Value));
	}

	Builder.CloseBrace();
}

bool FCSGenerator::CanExportFunction(const UStruct* Struct, const UFunction* Function) const
{
	if (DenyList.HasFunction(Struct, Function) || (!AllowList.HasFunction(Struct, Function) && !ShouldExportFunction(Function)))
	{
		return false;
	}

	if (Struct->IsChildOf(UBlueprintAsyncActionBase::StaticClass()))
	{
		const UClass* OwnerClass = Function->GetOwnerClass();
		const FProperty* ReturnProperty = Function->GetReturnProperty();
		if (OwnerClass && ReturnProperty)
		{
			const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(ReturnProperty);
			if (ObjectProperty && ObjectProperty->PropertyClass->IsChildOf(OwnerClass))
			{
				return true;
			}
		}
	}

	if (Function->HasMetaData(MD_Latent) || Function->HasMetaData(MD_BlueprintInternalUseOnly))
	{
		return BlueprintInternalAllowList.HasFunction(Struct, Function);
	}

	return CanExportFunctionParameters(Function);
}

bool FCSGenerator::CanExportFunctionParameters(const UFunction* Function) const
{
	const FProperty* ReturnProperty = Function->GetReturnProperty();
	
	if (ReturnProperty != nullptr && !CanExportReturnValue(ReturnProperty))
	{
		return false;
	}

	for (TFieldIterator<FProperty> ParamIt(Function); ParamIt && !ParamIt->HasAnyPropertyFlags(CPF_ReturnParm); ++ParamIt)
	{
		if (!CanExportParameter(*ParamIt))
		{
			return false;
		}
	}

	return true;
}

bool FCSGenerator::CanExportParameter(const FProperty* Property) const
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslatorManager->Find(Property);
		if (!Handler.IsSupportedAsParameter() || !Handler.CanHandleProperty(Property))
		{
			bCanExport = false;
		}
	}

	return bCanExport;
}

bool FCSGenerator::CanExportProperty(const UStruct* Struct, const FProperty* Property) const
{
	// Always include UProperties for whitelisted structs.
	// If their properties where blueprint-exposed, we wouldn't have had to whitelist them!
	bool bCanExport = !DenyList.HasProperty(Struct, Property)
	&& (CanExportPropertyShared(Property)
	|| AllowList.HasProperty(Struct, Property)
	|| AllowList.HasStruct(Struct));
	
	if (bCanExport)
	{
		const bool bIsClassProperty = Struct->IsA(UClass::StaticClass());
		check(bIsClassProperty || Struct->IsA(UScriptStruct::StaticClass()));

		const FPropertyTranslator& Handler = PropertyTranslatorManager->Find(Property);
		if ((bIsClassProperty && !Handler.IsSupportedAsProperty()) || (!bIsClassProperty && !Handler.IsSupportedAsStructProperty())  || !Handler.CanHandleProperty(Property))
		{
			bCanExport = false;
		}
	}

	return bCanExport;
}

bool FCSGenerator::CanExportPropertyShared(const FProperty* Property) const
{
	const FPropertyTranslator& Handler = PropertyTranslatorManager->Find(Property);

	// must be blueprint visible, should not be deprecated, arraydim == 1
	//if it's CPF_BlueprintVisible, we know it's RF_Public, CPF_Protected or MD_AllowPrivateAccess
	const bool bCanExport = ShouldExportProperty(Property)
		&& !Property->HasAnyPropertyFlags(CPF_Deprecated)
		&& (Property->ArrayDim == 1 || (Handler.IsSupportedInStaticArray() && Property->GetOutermost()->IsA(UClass::StaticClass())));

	return bCanExport;
}

void FCSGenerator::GetExportedProperties(TSet<FProperty*>& ExportedProperties, const UStruct* Struct)
{
	for (TFieldIterator<FProperty> PropertyIt(Struct, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		
		if (!CanExportProperty(Struct, Property))
		{
			continue;
		}

		ExportedProperties.Add(Property);
	}
}

void FCSGenerator::GetExportedFunctions(TSet<UFunction*>& ExportedFunctions, TSet<UFunction*>& ExportedOverridableFunctions, const UClass* Class)
{
	for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;
		
		if (!CanExportFunction(Class, Function))
		{
			continue;
		}
		
		if (Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
		{
			ExportedOverridableFunctions.Add(Function);
		}
		else
		{
			ExportedFunctions.Add(Function);
		}
	}
	
	for (const FImplementedInterface& Interface : Class->Interfaces)
	{
		for (TFieldIterator<UFunction> It(Interface.Class); It; ++It)
		{
			UFunction* Function = *It;
			
			if (!CanExportFunction(Class, Function) || !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent))
			{
				continue;
			}

			bool bIsOverriden = false;
			for (UFunction* ExportedOverridableFunction : ExportedOverridableFunctions)
			{
				if (Function->GetFName() == ExportedOverridableFunction->GetFName())
				{
					bIsOverriden = true;
					break;
				}
			}

			if (bIsOverriden)
			{
				continue;
			}
			
			ExportedOverridableFunctions.Add(Function);
		}
	}
}

void FCSGenerator::GetExportedStructs(TSet<UScriptStruct*>& ExportedStructs) const
{
	for(TObjectIterator<UScriptStruct> ScriptStructItr; ScriptStructItr; ++ScriptStructItr)
	{
		UScriptStruct* Struct = *ScriptStructItr;
		
		if (DenyList.HasStruct(Struct))
		{
			continue;
		}

		ExportedStructs.Add(Struct);
	}
}

bool FCSGenerator::CanExportOverridableParameter(const FProperty* Property)
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslatorManager->Find(Property);
		bCanExport = Handler.IsSupportedAsOverridableFunctionParameter() && Handler.CanHandleProperty(Property);
	}

	return bCanExport;
}

bool FCSGenerator::CanExportOverridableReturnValue(const FProperty* Property)
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslatorManager->Find(Property);
		if (!Handler.IsSupportedAsOverridableFunctionReturnValue() || !Handler.CanHandleProperty(Property))
		{
			bCanExport = false;
		}
	}

	return bCanExport;
}

const FString& FCSGenerator::GetNamespace(const UObject* Object)
{
	const FCSModule& Module = FindOrRegisterModule(Object);
	return Module.GetNamespace();
}

void FCSGenerator::RegisterClassToModule(const UObject* Struct)
{
	FindOrRegisterModule(Struct);
}

FCSModule& FCSGenerator::FindOrRegisterModule(const UObject* Struct)
{
	const FName ModuleName = UUnrealSharpStatics::GetModuleName(Struct);
	FCSModule* BindingsModule = CSharpBindingsModules.Find(ModuleName);
    
	if (!BindingsModule)
	{
		FString Directory = TEXT("");
		FString ProjectDirectory = FPaths::ProjectDir();
		FString GeneratedUserContent = "Script/obj/Generated";

		if (TSharedPtr<IPlugin> ThisPlugin = IPluginManager::Get().FindPlugin(UE_PLUGIN_NAME))
		{
			// If this plugin is a project plugin, we want to generate all the bindings in the same directory as the plug-in
			// since there's no reason to split the project from the plug-in, like you would need to if this was installed
			// as an engine plugin.
			if (ThisPlugin->GetType() == EPluginType::Project)
			{
				Directory = GeneratedScriptsDirectory;
			}
			else
			{
				if (TSharedPtr<IPlugin> Plugin = IPluginManager::Get().GetModuleOwnerPlugin(*ModuleName.ToString()))
				{
					if (Plugin->GetType() == EPluginType::Engine || Plugin->GetType() == EPluginType::Enterprise)
					{
						Directory = GeneratedScriptsDirectory;
					}
					else
					{
						Directory = FPaths::Combine(ProjectDirectory, GeneratedUserContent);
					}
				}
				else
				{
					if (IModuleInterface* Module = FModuleManager::Get().GetModule(ModuleName))
					{
						if (Module->IsGameModule())
						{
							Directory = FPaths::Combine(ProjectDirectory, GeneratedUserContent);
						}
						else
						{
							Directory = GeneratedScriptsDirectory;
						}
					}
					else
					{
						// This is awful, but we have no way of knowing if the module is a game module or not without loading it.
						// Also for whatever reason "CoreOnline" is not a module.
						Directory = GeneratedScriptsDirectory;
					}
				}
			}
		}

		ensureMsgf(!Directory.IsEmpty(), TEXT("Generating the directory location for generating the scripts for this module failed."));

		BindingsModule = &CSharpBindingsModules.Emplace(ModuleName, FCSModule(ModuleName, Directory));
	}

	return *BindingsModule;
}

void FCSGenerator::CheckGlueGeneratorVersion() const
{
	if (IFileManager::Get().DirectoryExists(*GeneratedScriptsDirectory))
	{
		int GlueGeneratorVersion = 0;
		GConfig->GetInt(GLUE_GENERATOR_CONFIG, GLUE_GENERATOR_VERSION_KEY, GlueGeneratorVersion, GEditorPerProjectIni);
		
		if (GlueGeneratorVersion < GLUE_GENERATOR_VERSION)
		{
			// Remove the whole generated folder if the version is different.
			// This is a bit of a sledgehammer, but it's the easiest way to ensure that we don't have any old files lying around.
			IFileManager::Get().DeleteDirectory(*GeneratedScriptsDirectory, false, true);
		}
	}
	
	GConfig->SetInt(GLUE_GENERATOR_CONFIG, GLUE_GENERATOR_VERSION_KEY, GLUE_GENERATOR_VERSION, GEditorPerProjectIni);
}

void FCSGenerator::ExportInterface(UClass* Interface, FCSScriptBuilder& Builder)
{
	if (!ensure(!ExportedTypes.Contains(Interface)))
	{
		return;
	}

	ExportedTypes.Add(Interface);

	FString InterfaceName = NameMapper.GetScriptClassName(Interface);
	const FCSModule& BindingsModule = FindOrRegisterModule(Interface);
	
	Builder.GenerateScriptSkeleton(BindingsModule.GetNamespace());
	AppendTooltip(Interface, Builder);
	Builder.DeclareType("interface", InterfaceName);
	
	Builder.AppendLine(FString::Printf(TEXT("public static readonly IntPtr NativeInterfaceClassPtr = UCoreUObjectExporter.CallGetNativeClassFromName(\"%s\");"), *Interface->GetName()));

	TSet<UFunction*> ExportedFunctions;
	TSet<UFunction*> ExportedOverridableFunctions;
	GetExportedFunctions(ExportedFunctions, ExportedOverridableFunctions, Interface);
	
	ExportInterfaceFunctions(Builder, Interface, ExportedOverridableFunctions);

	Builder.CloseBrace();

	Builder.AppendLine();
	Builder.AppendLine(FString::Printf(TEXT("public static class %sMarshaller"), *InterfaceName));
	Builder.OpenBrace();
	Builder.AppendLine(FString::Printf(TEXT("public static void ToNative(IntPtr nativeBuffer, int arrayIndex, %s obj)"), *InterfaceName));
	Builder.OpenBrace();
	Builder.AppendLine("	if (obj is CoreUObject.Object objectPointer)");
	Builder.AppendLine("	{");
	Builder.AppendLine("		InterfaceData data = new InterfaceData();");
	Builder.AppendLine("		data.ObjectPointer = objectPointer.NativeObject;");
	Builder.AppendLine(FString::Printf(TEXT("		data.InterfacePointer = %s.NativeInterfaceClassPtr;"), *InterfaceName));
	Builder.AppendLine("		BlittableMarshaller<InterfaceData>.ToNative(nativeBuffer, arrayIndex, data);");
	Builder.AppendLine("	}");
	Builder.CloseBrace();
	Builder.AppendLine();

	Builder.AppendLine(FString::Printf(TEXT("public static %s FromNative(IntPtr nativeBuffer, int arrayIndex)"), *InterfaceName));
	Builder.OpenBrace();
	Builder.AppendLine("	InterfaceData interfaceData = BlittableMarshaller<InterfaceData>.FromNative(nativeBuffer, arrayIndex);");
	Builder.AppendLine("	CoreUObject.Object unrealObject = ObjectMarshaller<CoreUObject.Object>.FromNative(interfaceData.ObjectPointer, 0);");
	Builder.AppendLine(FString::Printf(TEXT("	return unrealObject as %s;"), *InterfaceName));
	Builder.CloseBrace();
	Builder.CloseBrace();
}

void FCSGenerator::ExportDelegate(UFunction* SignatureFunction, FCSScriptBuilder& Builder)
{
	if (!ensure(!ExportedDelegates.Contains(SignatureFunction)))
	{
		return;
	}

	ensure(SignatureFunction->HasAnyFunctionFlags(FUNC_Delegate));

	ExportedDelegates.Add(SignatureFunction);

	FCSModule& Module = FCSGenerator::Get().FindOrRegisterModule(SignatureFunction->GetOutermost());
	FString DelegateName = FDelegateBasePropertyTranslator::GetDelegateName(SignatureFunction);

	Builder.GenerateScriptSkeleton(Module.GetNamespace());
	Builder.AppendLine();

	FString SignatureName = FString::Printf(TEXT("%s.Signature"), *DelegateName);
	FString SuperClass;
	if (SignatureFunction->HasAnyFunctionFlags(FUNC_MulticastDelegate))
	{
		SuperClass = FString::Printf(TEXT("MulticastDelegate<%s>"), *SignatureName);
	}
	else
	{
		SuperClass = FString::Printf(TEXT("Delegate<%s>"), *SignatureName);
	}

	Builder.DeclareType("class", DelegateName, SuperClass, true);

	PropertyTranslatorManager->Find(SignatureFunction).ExportDelegateFunction(Builder, SignatureFunction);

	// Write delegate initializer
	Builder.AppendLine("static public void InitializeUnrealDelegate(IntPtr nativeDelegateProperty)");
	Builder.OpenBrace();
	FCSGenerator::Get().ExportDelegateFunctionStaticConstruction(Builder, SignatureFunction);
	Builder.CloseBrace();

	Builder.CloseBrace();
}

void FCSGenerator::ExportClass(UClass* Class, FCSScriptBuilder& Builder)
{
	if (!ensure(!ExportedTypes.Contains(Class)))
	{
		return;
	}

	ExportedTypes.Add(Class);

	Builder.AppendLine(TEXT("// This file is automatically generated"));
	
	UClass* SuperClass = Class->GetSuperClass();
	if (SuperClass)
	{
		GenerateGlueForType(SuperClass, true);
	}
	
	const FString ScriptClassName = NameMapper.GetScriptClassName(Class);
	const FCSModule& BindingsModule = FindOrRegisterModule(Class);
	
	TSet<FProperty*> ExportedProperties;
	TSet<UFunction*> ExportedFunctions;
	TSet<UFunction*> ExportedOverridableFunctions;

	GetExportedProperties(ExportedProperties, Class);
	GetExportedFunctions(ExportedFunctions, ExportedOverridableFunctions, Class);

	TArray<FString> Interfaces;
	for (const FImplementedInterface& ImplementedInterface : Class->Interfaces)
	{
		UClass* InterfaceClass = ImplementedInterface.Class;
		Interfaces.Add(InterfaceClass->GetName());
	}

	TSet<const FCSModule*> Dependencies;
	GatherModuleDependencies(Class, Dependencies);

	for (const FCSModule* DependencyModule : Dependencies)
	{
		const FString& DelegateNamespace = DependencyModule->GetNamespace();
		Builder.DeclareDirective(DelegateNamespace);
	}

	Builder.GenerateScriptSkeleton(BindingsModule.GetNamespace());
	AppendTooltip(Class, Builder);
	FString Abstract = Class->HasAnyClassFlags(CLASS_Abstract) ? "ClassFlags.Abstract" : "";
	Builder.AppendLine(FString::Printf(TEXT("[UClass(%s)]"), *Abstract));
	Builder.DeclareType("class", ScriptClassName, GetSuperClassName(Class), true, Interfaces);

	TSet<FString> ReservedNames;
	for (const UFunction* Function : ExportedFunctions)
	{
		ReservedNames.Add(NameMapper.MapFunctionName(Function));
	}
	for (const UFunction* Function : ExportedOverridableFunctions)
	{
		ReservedNames.Add(NameMapper.MapFunctionName(Function));
	}
	for (const FProperty* Property : ExportedProperties)
	{
		ReservedNames.Add(NameMapper.ScriptifyName(Property->GetName(), EScriptNameKind::Property));
	}

	// Generate static constructor
	Builder.AppendLine();
	ExportStaticConstructor(Builder, Class, ExportedProperties, ExportedFunctions, ExportedOverridableFunctions, ReservedNames);

	ExportClassProperties(Builder, Class, ExportedProperties, ReservedNames);
	ExportClassFunctions(Builder, Class, ExportedFunctions);
	ExportClassOverridableFunctions(Builder, ExportedOverridableFunctions);	
	
	Builder.AppendLine();
	Builder.CloseBrace();
}

void FCSGenerator::ExportClassOverridableFunctions(FCSScriptBuilder& Builder, const TSet<UFunction*>& ExportedOverridableFunctions)
{
	for (UFunction* Function : ExportedOverridableFunctions)
	{
		PropertyTranslatorManager->Find(Function).ExportOverridableFunction(Builder, Function);
	}
}

void FCSGenerator::ExportClassFunctions(FCSScriptBuilder& Builder, const UClass* Class, const TSet<UFunction*>& ExportedFunctions)
{
	for (UFunction* Function : ExportedFunctions)
	{
		FPropertyTranslator::FunctionType FuncType = FPropertyTranslator::FunctionType::Normal;

		if (OverrideInternalList.HasFunction(Class, Function))
		{
			FuncType = FPropertyTranslator::FunctionType::InternalWhitelisted;
		}
		
		if (Function->HasAnyFunctionFlags(FUNC_Static) && Class->IsChildOf(UBlueprintFunctionLibrary::StaticClass()))
		{
			ExtensionMethod Method;
			if (GetExtensionMethodInfo(Method, Function))
			{
				const FCSModule& BindingsModule = FindOrRegisterModule(Class);
				TArray<ExtensionMethod>& ModuleExtensionMethods = ExtensionMethods.FindOrAdd(BindingsModule.GetModuleName());
				ModuleExtensionMethods.Add(Method);
			}
		}
		
		PropertyTranslatorManager->Find(Function).ExportFunction(Builder, Function, FuncType);
	}
}

void FCSGenerator::ExportInterfaceFunctions(FCSScriptBuilder& Builder, const UClass* Class, const TSet<UFunction*>& ExportedFunctions) const
{
	for (UFunction* Function : ExportedFunctions)
	{
		PropertyTranslatorManager->Find(Function).ExportInterfaceFunction(Builder, Function);
	}
}

void FCSGenerator::ExportClassProperties(FCSScriptBuilder& Builder, const UClass* Class, TSet<FProperty*>& ExportedProperties, const TSet<FString>& ReservedNames)
{
	for (FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyTranslator = PropertyTranslatorManager->Find(Property);
		PropertyTranslator.ExportWrapperProperty(Builder, Property, AllowList.HasProperty(Class, Property), ReservedNames);
	}
}

void FCSGenerator::ExportStaticConstructor(FCSScriptBuilder& Builder, const UStruct* Struct ,const TSet<FProperty*>& ExportedProperties,  const TSet<UFunction*>& ExportedFunctions, const TSet<UFunction*>& ExportedOverrideableFunctions, const TSet<FString>& ReservedNames)
{
	const UClass* Class = Cast<UClass>(Struct);
	const UScriptStruct* ScriptStruct = Cast<UScriptStruct>(Struct);

	if (!ScriptStruct)
	{
		if (ExportedProperties.IsEmpty() && ExportedFunctions.IsEmpty() && ExportedOverrideableFunctions.IsEmpty())
		{
			return;
		}
	}
	
	bool bHasStaticFunctions = false;
	
	for (UFunction* ExportedFunction : ExportedFunctions)
	{
		if (ExportedFunction->HasAnyFunctionFlags(FUNC_Static))
		{
			bHasStaticFunctions = true;
			break;
		}
	}

	if (bHasStaticFunctions)
	{
		// Keep the class pointer so we can use the CDO to invoke static functions.
		Builder.AppendLine("static readonly IntPtr NativeClassPtr;");
	}

	if (ScriptStruct)
	{
		Builder.AppendLine("public static readonly int NativeDataSize;");
	}

	FString TypeName = NameMapper.GetTypeScriptName(Struct);
	
	Builder.AppendLine(FString::Printf(TEXT("static %s()"), *TypeName));
	Builder.OpenBrace();

	Builder.AppendLine(FString::Printf(TEXT("%sNativeClassPtr = %s.CallGetNative%sFromName(\"%s\");"),
		bHasStaticFunctions ? TEXT("") : TEXT("IntPtr "),
		CoreUObjectCallbacks,
		Class ? TEXT("Class") : TEXT("Struct"), 
		*Struct->GetName()));

	Builder.AppendLine();

	ExportPropertiesStaticConstruction(Builder, ExportedProperties, ReservedNames);

	if (Class)
	{
		Builder.AppendLine();
		ExportClassFunctionsStaticConstruction(Builder, ExportedFunctions);
		Builder.AppendLine();
		ExportClassOverridableFunctionsStaticConstruction(Builder, ExportedOverrideableFunctions);
		Builder.AppendLine();
	}
	else
	{
		Builder.AppendLine();
		Builder.AppendLine(FString::Printf(TEXT("NativeDataSize = %s.CallGetNativeStructSize(NativeClassPtr);"), UScriptStructCallbacks));
	}

	Builder.CloseBrace();
}

void FCSGenerator::ExportClassOverridableFunctionsStaticConstruction(FCSScriptBuilder& Builder, const TSet<UFunction*>& ExportedOverridableFunctions) const
{
	for (UFunction* Function : ExportedOverridableFunctions)
	{
		if (Function->NumParms == 0)
		{
			continue;
		}

		bool bIsEditorOnly = Function->HasAnyFunctionFlags(FUNC_EditorOnly);
		
		if (bIsEditorOnly)
		{
			Builder.BeginWithEditorOnlyBlock();
		}
		
		FString NativeMethodName = Function->GetName();
		Builder.AppendLine(FString::Printf(TEXT("IntPtr %s_NativeFunction = %s.CallGetNativeFunctionFromClassAndName(NativeClassPtr, \"%s\");"), *NativeMethodName, UClassCallbacks, *NativeMethodName));
		Builder.AppendLine(FString::Printf(TEXT("%s_ParamsSize = %s.CallGetNativeFunctionParamsSize(%s_NativeFunction);"), *NativeMethodName, UFunctionCallbacks, *NativeMethodName));
		for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
		{
			FProperty* Property = *It;
			const FPropertyTranslator& ParamHandler = PropertyTranslatorManager->Find(Property);
			ParamHandler.ExportParameterStaticConstruction(Builder, NativeMethodName, Property);
		}

		if (bIsEditorOnly)
		{
			Builder.EndPreprocessorBlock();
		}

		Builder.AppendLine();
	}
}

void FCSGenerator::ExportClassFunctionsStaticConstruction(FCSScriptBuilder& Builder, const TSet<UFunction*>& ExportedFunctions)
{
	for (const UFunction* Function : ExportedFunctions)
	{
		ExportClassFunctionStaticConstruction(Builder, Function);
	}
}

void FCSGenerator::ExportClassFunctionStaticConstruction(FCSScriptBuilder& Builder, const UFunction *Function)
{
	FString NativeMethodName = Function->GetName();
	bool bIsEditorOnly = Function->HasAnyFunctionFlags(FUNC_EditorOnly);
	
	if (bIsEditorOnly)
	{
		Builder.BeginWithEditorOnlyBlock();
	}
	
	Builder.AppendLine(FString::Printf(TEXT("%s_NativeFunction = %s.CallGetNativeFunctionFromClassAndName(NativeClassPtr, \"%s\");"), *NativeMethodName, UClassCallbacks, *Function->GetName()));
	
	if (Function->NumParms > 0)
	{
		Builder.AppendLine(FString::Printf(TEXT("%s_ParamsSize = %s.CallGetNativeFunctionParamsSize(%s_NativeFunction);"), *NativeMethodName, UFunctionCallbacks, *NativeMethodName));
	}
	
	for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		const FPropertyTranslator& ParamHandler = PropertyTranslatorManager->Find(Property);
		ParamHandler.ExportParameterStaticConstruction(Builder, NativeMethodName, Property);
	}

	if (bIsEditorOnly)
	{
		Builder.EndPreprocessorBlock();
	}
}

void FCSGenerator::ExportDelegateFunctionStaticConstruction(FCSScriptBuilder& Builder, const UFunction* Function)
{
	FString NativeMethodName = Function->GetName();
	Builder.AppendLine(FString::Printf(TEXT("%s_NativeFunction = FMulticastDelegatePropertyExporter.CallGetSignatureFunction(nativeDelegateProperty);"), *NativeMethodName));
	if (Function->NumParms > 0)
	{
		Builder.AppendLine(FString::Printf(TEXT("%s_ParamsSize = %s.CallGetNativeFunctionParamsSize(%s_NativeFunction);"), *NativeMethodName, UFunctionCallbacks, *NativeMethodName));
	}
	
	for (TFieldIterator<FProperty> It(Function, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		FProperty* Property = *It;
		const FPropertyTranslator& ParamHandler = PropertyTranslatorManager->Find(Property);
		ParamHandler.ExportParameterStaticConstruction(Builder, NativeMethodName, Property);
	}
}

void FCSGenerator::ExportPropertiesStaticConstruction(FCSScriptBuilder& Builder, const TSet<FProperty*>& ExportedProperties, const TSet<FString>& ReservedNames)
{
	//we already warn on conflicts when exporting the properties themselves, so here we can just silently skip them
	TSet<FString> ExportedPropertiesHash;

	for (FProperty* Property : ExportedProperties)
	{
		FString ManagedName = NameMapper.MapPropertyName(Property, ReservedNames);
		
		if (ExportedPropertiesHash.Contains(ManagedName))
		{
			continue;
		}

		if (Property->HasAnyPropertyFlags(CPF_EditorOnly))
		{
			Builder.BeginWithEditorOnlyBlock();
		}
		
		ExportedPropertiesHash.Add(ManagedName);
		PropertyTranslatorManager->Find(Property).ExportPropertyStaticConstruction(Builder, Property, Property->GetName());

		if (Property->HasAnyPropertyFlags(CPF_EditorOnly))
		{
			Builder.EndPreprocessorBlock();
		}
	}
}

bool FCSGenerator::GetExtensionMethodInfo(ExtensionMethod& Info, UFunction* Function)
{
	// ScriptMethod is the canonical metadata for extension methods
	if (!Function->HasMetaData("ExtensionMethod") || Function->NumParms == 0)
	{
		return false;
	}

	FObjectProperty* SelfParameter = CastField<FObjectProperty>(Function->ChildProperties);

	if (!SelfParameter)
	{
		return false;
	}
	
	Info.Function = Function;
	Info.SelfParameter = SelfParameter;
	Info.OverrideClassBeingExtended = SelfParameter->PropertyClass;
	return true;
}

void FCSGenerator::ExportStruct(UScriptStruct* Struct, FCSScriptBuilder& Builder)
{
	const FCSModule& BindingsModule = FindOrRegisterModule(Struct);

	TSet<FProperty*> ExportedProperties;
	if (UStruct* ParentStruct = Struct->GetSuperStruct())
	{
		GetExportedProperties(ExportedProperties, ParentStruct);
	}
	GetExportedProperties(ExportedProperties, Struct);
	
	Builder.GenerateScriptSkeleton(BindingsModule.GetNamespace());

	const bool bIsBlittable = PropertyTranslatorManager->IsStructBlittable(*Struct);
	
	FCSPropertyBuilder PropBuilder;

	PropBuilder.AddAttribute("UStruct");
		
	if (bIsBlittable)
	{
		PropBuilder.AddArgument("IsBlittable = true");
	}
	
	PropBuilder.Finish();

	AppendTooltip(Struct, Builder);
	Builder.AppendLine(PropBuilder.ToString());
	Builder.DeclareType("struct", NameMapper.GetStructScriptName(Struct));

	TSet<FString> ReservedNames;
	for (const FProperty* Property : ExportedProperties)
	{
		ReservedNames.Add(Property->GetName());
	}
	
	ExportStructProperties(Builder, ExportedProperties, bIsBlittable, ReservedNames);

	if (!bIsBlittable)
	{
		// Generate static constructor
		Builder.AppendLine();
		
		ExportStaticConstructor(Builder, Struct, ExportedProperties, {}, {}, ReservedNames);

		// Generate native constructor
		Builder.AppendLine();
		ExportMirrorStructMarshalling(Builder, Struct, ExportedProperties, ReservedNames);
	}
	
	Builder.CloseBrace();

	if (!bIsBlittable)
	{
		// Generate custom marshaler for arrays of this struct
		ExportStructMarshaller(Builder, Struct);
	}
}


void FCSGenerator::ExportStructProperties(FCSScriptBuilder& Builder, const TSet<FProperty*>& ExportedProperties, bool bSuppressOffsets, const TSet<FString>& ReservedNames) const
{
	for (FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyTranslator = PropertyTranslatorManager->Find(Property);
		PropertyTranslator.ExportMirrorProperty(Builder, Property, bSuppressOffsets, ReservedNames);
	}
}

void FCSGenerator::ExportStructMarshaller(FCSScriptBuilder& Builder, const UScriptStruct* Struct)
{
	FString StructName = NameMapper.GetStructScriptName(Struct);

	Builder.AppendLine();
	Builder.AppendLine(FString::Printf(TEXT("public static class %sMarshaller"), *StructName));
	Builder.OpenBrace();

	Builder.AppendLine(FString::Printf(TEXT("public static %s FromNative(IntPtr nativeBuffer, int arrayIndex)"), *StructName));
	Builder.OpenBrace();
	Builder.AppendLine(FString::Printf(TEXT("return new %s(nativeBuffer + arrayIndex * GetNativeDataSize());"), *StructName));
	Builder.CloseBrace(); // MarshalNativeToManaged

	Builder.AppendLine();
	Builder.AppendLine(FString::Printf(TEXT("public static void ToNative(IntPtr nativeBuffer, int arrayIndex, %s obj)"), *StructName));
	Builder.OpenBrace();
	Builder.AppendLine("obj.ToNative(nativeBuffer + arrayIndex * GetNativeDataSize());");
	Builder.CloseBrace(); // MarshalManagedToNative

	Builder.AppendLine();
	Builder.AppendLine("public static int GetNativeDataSize()");
	Builder.OpenBrace();
	Builder.AppendLine(FString::Printf(TEXT("return %s.NativeDataSize;"), *StructName));
	Builder.CloseBrace();
	Builder.CloseBrace();
}

void FCSGenerator::ExportMirrorStructMarshalling(FCSScriptBuilder& Builder, const UScriptStruct* Struct, TSet<FProperty*> ExportedProperties, const TSet<FString>& ReservedNames) const
{
	Builder.AppendLine();
	Builder.AppendLine("// Construct by marshalling from a native buffer.");
	Builder.AppendLine(FString::Printf(TEXT("public %s(IntPtr InNativeStruct)"), *NameMapper.GetStructScriptName(Struct)));
	Builder.OpenBrace();
	Builder.BeginUnsafeBlock();

	for (FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyHandler = PropertyTranslatorManager->Find(Property);
		FString NativePropertyName = Property->GetName();
		FString CSharpPropertyName = NameMapper.MapPropertyName(Property, ReservedNames);
		PropertyHandler.ExportMarshalFromNativeBuffer(
			Builder, 
			Property, 
			NativePropertyName,
			FString::Printf(TEXT("%s ="), *CSharpPropertyName),
			"InNativeStruct",
			FString::Printf(TEXT("%s_Offset"),*NativePropertyName), 
			false,
			false);
	}

	Builder.EndUnsafeBlock();
	Builder.CloseBrace(); // ctor
	
	Builder.AppendLine();
	Builder.AppendLine("// Marshal into a preallocated native buffer.");
	Builder.AppendLine("public void ToNative(IntPtr buffer)");
	Builder.OpenBrace();
	Builder.BeginUnsafeBlock();

	for (const FProperty* Property : ExportedProperties)
	{
		const FPropertyTranslator& PropertyHandler = PropertyTranslatorManager->Find(Property);
		FString NativePropertyName = Property->GetName();
		FString CSharpPropertyName = NameMapper.MapPropertyName(Property, ReservedNames);
		PropertyHandler.ExportMarshalToNativeBuffer(
			Builder, 
			Property, 
			NativePropertyName,
			"buffer",
			FString::Printf(TEXT("%s_Offset"), *NativePropertyName),
			CSharpPropertyName);
	}

	Builder.EndUnsafeBlock();
	Builder.CloseBrace(); // ToNative
}

FString FCSGenerator::GetSuperClassName(const UClass* Class) const
{
	if (Class == UObject::StaticClass())
	{
		return UNREAL_SHARP_OBJECT;
	}

	// For all other classes, return the fully qualified name of the superclass
	const UClass* SuperClass = Class->GetSuperClass();
	return NameMapper.GetQualifiedName(SuperClass);
}

void FCSGenerator::SaveTypeGlue(const UPackage* Package, const FString& TypeName, const FCSScriptBuilder& ScriptBuilder)
{
	const FString FileName = FString::Printf(TEXT("%s.generated.cs"), *TypeName);
	SaveGlue(FindOrRegisterModule(Package), FileName, ScriptBuilder.ToString());
}

void FCSGenerator::SaveGlue(const FCSModule& Bindings, const FString& Filename, const FString& GeneratedGlue)
{
	const FString& BindingsSourceDirectory = Bindings.GetGeneratedSourceDirectory();

	IPlatformFile& File = FPlatformFileManager::Get().GetPlatformFile();
	if (!File.CreateDirectoryTree(*BindingsSourceDirectory))
	{
		UE_LOG(LogGlueGenerator, Error, TEXT("Could not create directory %s"), *BindingsSourceDirectory);
		return;
	}

	const FString GlueOutputPath = FPaths::Combine(*BindingsSourceDirectory, *Filename);
	GeneratedFileManager.SaveFileIfChanged(GlueOutputPath, GeneratedGlue);
}

bool FCSGenerator::CanExportReturnValue(const FProperty* Property) const
{
	bool bCanExport = Property->ArrayDim == 1;

	if (bCanExport)
	{
		const FPropertyTranslator& Handler = PropertyTranslatorManager->Find(Property);
		if (!Handler.IsSupportedAsReturnValue() || !Handler.CanHandleProperty(Property))
		{
			bCanExport = false;
		}
	}

	return bCanExport;
}

void FCSGenerator::GatherModuleDependencies(const FProperty* Property, TSet<const FCSModule*>& DependencySet)
{
	TSet<UFunction*> DelegateSignatures;
	PropertyTranslatorManager->Find(Property).AddDelegateReferences(Property, DelegateSignatures);

	for (UFunction* DelegateSignature : DelegateSignatures)
	{
		const FCSModule& DelegateModule = FCSGenerator::Get().FindOrRegisterModule(DelegateSignature->GetOutermost());

		DependencySet.Add(&DelegateModule);
	}
}

void FCSGenerator::GatherModuleDependencies(const UClass* Class, TSet<const FCSModule*>& DependencySet)
{
	// Gather the modules for all interfaces that the class implements
	for (const FImplementedInterface& ImplementedInterface : Class->Interfaces)
	{
		UClass* InterfaceClass = ImplementedInterface.Class;
		const FCSModule& InterfaceModule = FindOrRegisterModule(InterfaceClass);

		DependencySet.Add(&InterfaceModule);
	}

	// Gather the module for the base class of the class
	const UClass* SuperClass = Class->GetSuperClass();
	if (SuperClass)
	{
		const FCSModule& SuperClassModule = FindOrRegisterModule(SuperClass);

		DependencySet.Add(&SuperClassModule);
	}

	// Gather the modules for the types of all properties that the class contains
	for (TFieldIterator<FProperty> PropertyIt(Class, EFieldIteratorFlags::ExcludeSuper); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;

		if (!CanExportProperty(Class, Property))
		{
			continue;
		}

		GatherModuleDependencies(Property, DependencySet);
	}

	// Gather the modules for the return and parameter types of all functions that the class contains
	for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;

		if (!CanExportFunction(Class, Function))
		{
			continue;
		}

		FProperty* ReturnProperty = Function->GetReturnProperty();
		if (ReturnProperty)
		{
			GatherModuleDependencies(ReturnProperty, DependencySet);
		}

		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt; ++ParamIt)
		{
			FProperty* Parameter = *ParamIt;
			GatherModuleDependencies(Parameter, DependencySet);
		}
	}
}

void FCSGenerator::AddExportedType(UObject* Object)
{
	ExportedTypes.Add(Object);
}
