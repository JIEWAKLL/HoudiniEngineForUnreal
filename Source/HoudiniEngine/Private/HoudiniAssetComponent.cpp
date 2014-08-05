/*
 * PROPRIETARY INFORMATION.  This software is proprietary to
 * Side Effects Software Inc., and is not to be reproduced,
 * transmitted, or disclosed in any way without written permission.
 *
 * Produced by:
 *      Damian Campeanu, Mykola Konyk
 *      Side Effects Software Inc
 *      123 Front Street West, Suite 1401
 *      Toronto, Ontario
 *      Canada   M5J 2M2
 *      416-504-9876
 *
 */

#include "HoudiniEnginePrivatePCH.h"
#include <stdint.h>


UScriptStruct*
UHoudiniAssetComponent::ScriptStructColor = nullptr;


uint32
UHoudiniAssetComponent::ComponentPatchedClassCounter = 0u;


// Define accessor for UObjectBase::SetClass private method.
struct FObjectBaseAccess
{
	typedef void (UObjectBase::*type)(UClass*);
};

HOUDINI_PRIVATE_PATCH(FObjectBaseAccess, UObjectBase::SetClass);



UHoudiniAssetComponent::UHoudiniAssetComponent(const FPostConstructInitializeProperties& PCIP) :
	Super(PCIP),
	HoudiniAsset(nullptr),
	PatchedClass(nullptr),
	AssetId(-1),
	bIsNativeComponent(false),
	bIsPreviewComponent(false),
	bAsyncResourceReleaseHasBeenStarted(false),
	bPreSaveTriggered(false),
	bLoadedComponent(false),
	bLoadedComponentRequiresInstantiation(false)
{
	// Create a generic bounding volume.
	HoudiniMeshSphereBounds = FBoxSphereBounds(FBox(-FVector(1.0f, 1.0f, 1.0f) * HALF_WORLD_MAX, FVector(1.0f, 1.0f, 1.0f) * HALF_WORLD_MAX));

	// Set component properties.
	Mobility = EComponentMobility::Movable;
	PrimaryComponentTick.bCanEverTick = true;
	bTickInEditor = true;
	bGenerateOverlapEvents = false;

	// Similar to UMeshComponent.
	CastShadow = true;
	bUseAsOccluder = true;
	bCanEverAffectNavigation = true;

	// This component requires render update.
	bNeverNeedsRenderUpdate = false;

	// Make an invalid GUID, since we do not have any cooking requests.
	HapiGUID.Invalidate();

	// Zero scratch space.
	FMemory::Memset(ScratchSpaceBuffer, 0x0, HOUDINIENGINE_ASSET_SCRATCHSPACE_SIZE);

	// Create temporary geometry.
	FHoudiniEngineUtils::GetHoudiniLogoGeometry(HoudiniMeshTriangles, HoudiniMeshSphereBounds);
}


void
UHoudiniAssetComponent::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	// We need to make sure the component class has been patched.
	UClass* ObjectClass = InThis->GetClass();

	if(UHoudiniAssetComponent::StaticClass() != ObjectClass)
	{
		if(ObjectClass->ClassAddReferencedObjects == UHoudiniAssetComponent::AddReferencedObjects)
		{
			// This is a safe cast since our component is the only type registered for this callback.
			UHoudiniAssetComponent* HoudiniAssetComponent = (UHoudiniAssetComponent*)InThis;
			if(HoudiniAssetComponent && !HoudiniAssetComponent->IsPendingKill())
			{
				// If we have patched class object, add it as referenced.
				UClass* PatchedClass = HoudiniAssetComponent->PatchedClass;
				if(PatchedClass)
				{
					Collector.AddReferencedObject(PatchedClass, InThis);
				}

				// Retrieve asset associated with this component.
				UHoudiniAsset* HoudiniAsset = HoudiniAssetComponent->GetHoudiniAsset();
				if(HoudiniAsset)
				{
					// Manually add a reference to Houdini asset from this component.
					Collector.AddReferencedObject(HoudiniAsset, InThis);
				}

				// Propagate referencing request to all geos.
				for(TArray<FHoudiniAssetObjectGeo*>::TIterator Iter = HoudiniAssetComponent->HoudiniAssetObjectGeos.CreateIterator(); Iter; ++Iter)
				{
					FHoudiniAssetObjectGeo* HoudiniAssetObjectGeo = *Iter;
					HoudiniAssetObjectGeo->AddReferencedObjects(Collector);
				}
			}
		}
	}

	// Call base implementation.
	Super::AddReferencedObjects(InThis, Collector);
}


void
UHoudiniAssetComponent::SetNative(bool InbIsNativeComponent)
{
	bIsNativeComponent = InbIsNativeComponent;
}


HAPI_AssetId
UHoudiniAssetComponent::GetAssetId() const
{
	return AssetId;
}


void
UHoudiniAssetComponent::SetAssetId(HAPI_AssetId InAssetId)
{
	AssetId = InAssetId;
}


UHoudiniAsset*
UHoudiniAssetComponent::GetHoudiniAsset() const
{
	return HoudiniAsset;
}


TWeakObjectPtr<AHoudiniAssetActor>
UHoudiniAssetComponent::GetHoudiniAssetActorOwner() const
{
	return HoudiniAssetActorOwner;
}


void
UHoudiniAssetComponent::SetHoudiniAsset(UHoudiniAsset* InHoudiniAsset)
{
	HOUDINI_LOG_MESSAGE(TEXT("Setting asset, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);

	UHoudiniAsset* Asset = nullptr;
	TWeakObjectPtr<AHoudiniAssetActor> HoudiniAssetActor = GetHoudiniAssetActorOwner();
	check(HoudiniAssetActor.IsValid());

	// If it is the same asset, do nothing.
	if(InHoudiniAsset == HoudiniAsset)
	{
		return;
	}

	HoudiniAsset = InHoudiniAsset;
	bIsPreviewComponent = HoudiniAssetActor->IsUsedForPreview();

	if(!InHoudiniAsset->DoesPreviewGeometryContainHoudiniLogo())
	{
		// If asset contains non logo geometry, retrieve it and use it.
		InHoudiniAsset->RetrievePreviewGeometry(HoudiniMeshTriangles);

		// Update rendering information.
		UpdateRenderingInformation();
	}

	if(!bIsPreviewComponent && !bLoadedComponent)
	{
		EHoudiniEngineTaskType::Type HoudiniEngineTaskType = EHoudiniEngineTaskType::AssetInstantiation;

		// Create new GUID to identify this request.
		HapiGUID = FGuid::NewGuid();

		FHoudiniEngineTask Task(HoudiniEngineTaskType, HapiGUID);
		Task.Asset = InHoudiniAsset;
		Task.ActorName = HoudiniAssetActor->GetActorLabel();
		FHoudiniEngine::Get().AddTask(Task);

		// Start ticking - this will poll the cooking system for completion.
		StartHoudiniTicking();
	}
}


void
UHoudiniAssetComponent::AssignUniqueActorLabel()
{
	if(-1 != AssetId)
	{
		TWeakObjectPtr<AHoudiniAssetActor> HoudiniAssetActor = GetHoudiniAssetActorOwner();
		if(HoudiniAssetActor.IsValid())
		{
			FString UniqueName;
			if(FHoudiniEngineUtils::GetHoudiniAssetName(AssetId, UniqueName))
			{
				GEditor->SetActorLabelUnique(HoudiniAssetActor.Get(), UniqueName);
			}
		}
	}
}


void
UHoudiniAssetComponent::ClearGeos()
{
	for(TArray<FHoudiniAssetObjectGeo*>::TIterator Iter = HoudiniAssetObjectGeos.CreateIterator(); Iter; ++Iter)
	{
		delete(*Iter);
	}

	HoudiniAssetObjectGeos.Empty();
}


bool
UHoudiniAssetComponent::ContainsGeos() const
{
	return (HoudiniAssetObjectGeos.Num() > 0);
}


void
UHoudiniAssetComponent::StartHoudiniTicking()
{
	// If we have no timer delegate spawned for this preview component, spawn one.
	if(!TimerDelegate.IsBound())
	{
		TimerDelegate = FTimerDelegate::CreateUObject(this, &UHoudiniAssetComponent::TickHoudiniComponent);

		// We need to register delegate with the timer system.
		static const float TickTimerDelay = 0.25f;
		GEditor->GetTimerManager()->SetTimer(TimerDelegate, TickTimerDelay, true);
	}
}


void
UHoudiniAssetComponent::StopHoudiniTicking()
{
	if(TimerDelegate.IsBound())
	{
		GEditor->GetTimerManager()->ClearTimer(TimerDelegate);
		TimerDelegate.Unbind();
	}
}


void
UHoudiniAssetComponent::TickHoudiniComponent()
{
	FHoudiniEngineTaskInfo TaskInfo;
	bool bStopTicking = false;

	// Retrieve the owner actor of this component.
	TWeakObjectPtr<AHoudiniAssetActor> HoudiniAssetActor = GetHoudiniAssetActorOwner();
	check(HoudiniAssetActor.IsValid());

	if(HapiGUID.IsValid())
	{
		// If we have a valid task GUID.

		if(FHoudiniEngine::Get().RetrieveTaskInfo(HapiGUID, TaskInfo))
		{
			if(EHoudiniEngineTaskState::None != TaskInfo.TaskState)
			{
				if(!NotificationPtr.IsValid())
				{
					FNotificationInfo Info(TaskInfo.StatusText);

					Info.bFireAndForget = false;
					Info.FadeOutDuration = 2.0f;
					Info.ExpireDuration = 2.0f;

					TSharedPtr<FSlateDynamicImageBrush> HoudiniBrush = FHoudiniEngine::Get().GetHoudiniLogoBrush();
					if(HoudiniBrush.IsValid())
					{
						Info.Image = HoudiniBrush.Get();
					}

					NotificationPtr = FSlateNotificationManager::Get().AddNotification(Info);
				}
			}

			switch(TaskInfo.TaskState)
			{
				case EHoudiniEngineTaskState::FinishedInstantiationWithoutCooking:
				{
					// Set new asset id.
					SetAssetId(TaskInfo.AssetId);

					if(-1 == TaskInfo.AssetId)
					{
						bStopTicking = true;
						HOUDINI_LOG_MESSAGE(TEXT("Received invalid asset id."));
					}

					// Otherwise we do not stop ticking, as we want to schedule a cook task right away (after submitting
					// all changed parameters).
					if(NotificationPtr.IsValid())
					{
						TSharedPtr<SNotificationItem> NotificationItem = NotificationPtr.Pin();
						if(NotificationItem.IsValid())
						{
							NotificationItem->SetText(TaskInfo.StatusText);
							NotificationItem->ExpireAndFadeout();

							NotificationPtr.Reset();
						}
					}
					FHoudiniEngine::Get().RemoveTaskInfo(HapiGUID);
					HapiGUID.Invalidate();

					break;
				}

				case EHoudiniEngineTaskState::FinishedInstantiation:
				case EHoudiniEngineTaskState::FinishedCooking:
				{
					if(-1 != TaskInfo.AssetId)
					{
						// Set new asset id.
						SetAssetId(TaskInfo.AssetId);

						// Assign unique actor label based on asset name.
						AssignUniqueActorLabel();

						if(FHoudiniEngineUtils::GetAssetGeometry(TaskInfo.AssetId, HoudiniMeshTriangles, HoudiniMeshSphereBounds))
						{
							// We need to patch component RTTI to reflect properties for this component.
							ReplaceClassInformation(HoudiniAssetActor->GetActorLabel());

							// Get current asset.
							UHoudiniAsset* CurrentHoudiniAsset = GetHoudiniAsset();

							// See if asset contains Houdini logo geometry, if it does we can update it.
							if(CurrentHoudiniAsset && CurrentHoudiniAsset->DoesPreviewGeometryContainHoudiniLogo())
							{
								CurrentHoudiniAsset->SetPreviewGeometry(HoudiniMeshTriangles);

								// We need to find corresponding preview component.
								for(TObjectIterator<UHoudiniAssetComponent> It; It; ++It)
								{
									UHoudiniAssetComponent* HoudiniAssetComponent = *It;

									// Skip ourselves.
									if(HoudiniAssetComponent == this)
									{
										continue;
									}

									if(HoudiniAssetComponent->HoudiniAsset && HoudiniAssetComponent->HoudiniAsset == CurrentHoudiniAsset)
									{
										// Update preview actor geometry with new data.
										HoudiniAssetComponent->HoudiniMeshTriangles = HoudiniMeshTriangles;
										HoudiniAssetComponent->UpdateRenderingInformation();

										break;
									}
								}
							}

							// Update properties panel.
							UpdateEditorProperties();

							// Construct new objects (asset objects and asset object parts).
							TArray<FHoudiniAssetObjectGeo*> NewObjectGeos;
							FHoudiniEngineUtils::ConstructGeos(AssetId, HoudiniAssetObjectGeos, NewObjectGeos);

							// Clear rendering resources used by geos.
							ReleaseRenderingResources();

							// Delete all existing geo objects (this will also delete their geo parts).
							ClearGeos();

							// Set new geo objects.
							HoudiniAssetObjectGeos = NewObjectGeos;

							// Collect all textures (for debugging purposes).
							CollectTextures();

							// Create all rendering resources.
							CreateRenderingResources();

							// Need to update rendering information.
							UpdateRenderingInformation();
						}
						else
						{
							HOUDINI_LOG_MESSAGE(TEXT("Failed geometry extraction after asset instantiation."));
						}
					}
					else
					{
						HOUDINI_LOG_MESSAGE(TEXT("Received invalid asset id."));
					}

					if(NotificationPtr.IsValid())
					{
						TSharedPtr<SNotificationItem> NotificationItem = NotificationPtr.Pin();
						if(NotificationItem.IsValid())
						{
							NotificationItem->SetText(TaskInfo.StatusText);
							NotificationItem->ExpireAndFadeout();

							NotificationPtr.Reset();
						}
					}

					FHoudiniEngine::Get().RemoveTaskInfo(HapiGUID);
					HapiGUID.Invalidate();

					bStopTicking = true;
					break;
				}

				case EHoudiniEngineTaskState::Aborted:
				case EHoudiniEngineTaskState::FinishedInstantiationWithErrors:
				case EHoudiniEngineTaskState::FinishedCookingWithErrors:
				case EHoudiniEngineTaskState::FinishedInstantiationWithoutCookingWithErrors:
				{
					HOUDINI_LOG_MESSAGE(TEXT("Failed asset instantiation."));

					if(NotificationPtr.IsValid())
					{
						TSharedPtr<SNotificationItem> NotificationItem = NotificationPtr.Pin();
						if(NotificationItem.IsValid())
						{
							NotificationItem->SetText(TaskInfo.StatusText);
							NotificationItem->ExpireAndFadeout();

							NotificationPtr.Reset();
						}
					}

					FHoudiniEngine::Get().RemoveTaskInfo(HapiGUID);
					HapiGUID.Invalidate();

					bStopTicking = true;
					break;
				}

				case EHoudiniEngineTaskState::Processing:
				{

					if(NotificationPtr.IsValid())
					{
						TSharedPtr<SNotificationItem> NotificationItem = NotificationPtr.Pin();
						if(NotificationItem.IsValid())
						{
							NotificationItem->SetText(TaskInfo.StatusText);
						}
					}

					break;
				}

				case EHoudiniEngineTaskState::None:
				default:
				{
					break;
				}
			}
		}
		else
		{
			// Task information does not exist, we can stop ticking.
			HapiGUID.Invalidate();
			bStopTicking = true;
		}
	}

	if(!HapiGUID.IsValid() && (ChangedProperties.Num() > 0))
	{
		// If we are not cooking and we have property changes queued up.

		// Create new GUID to identify this request.
		HapiGUID = FGuid::NewGuid();

		if(bLoadedComponentRequiresInstantiation)
		{
			bLoadedComponentRequiresInstantiation = false;

			FHoudiniEngineTask Task(EHoudiniEngineTaskType::AssetInstantiationWithoutCooking, HapiGUID);
			Task.Asset = HoudiniAsset;
			Task.ActorName = HoudiniAssetActor->GetActorLabel();
			FHoudiniEngine::Get().AddTask(Task);
		}
		else
		{
			// We need to set all parameter values which have changed.
			SetChangedParameterValues();

			// Remove all processed parameters.
			ChangedProperties.Empty();

			// Create asset instantiation task object and submit it for processing.
			FHoudiniEngineTask Task(EHoudiniEngineTaskType::AssetCooking, HapiGUID);
			Task.ActorName = HoudiniAssetActor->GetActorLabel();
			Task.AssetComponent = this;
			FHoudiniEngine::Get().AddTask(Task);
		}

		// We do not want to stop ticking system as we have just submitted a task.
		bStopTicking = false;
	}

	if(bStopTicking)
	{
		StopHoudiniTicking();
	}
}


void
UHoudiniAssetComponent::UpdateEditorProperties()
{
	TWeakObjectPtr<AHoudiniAssetActor> HoudiniAssetActor = GetHoudiniAssetActorOwner();
	if(HoudiniAssetActor.IsValid())
	{
		// Manually reselect the actor - this will cause details panel to be updated and force our 
		// property changes to be picked up by the UI.
		//GEditor->SelectActor(HoudiniAssetActor.Get(), true, true);

		// Notify the editor about selection change.
		//GEditor->NoteSelectionChange();
	}
}


FBoxSphereBounds
UHoudiniAssetComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	return HoudiniMeshSphereBounds;
}


void
UHoudiniAssetComponent::CollectTextures()
{
	HoudiniTextures.Reset();

	for(TArray<FHoudiniAssetObjectGeo*>::TIterator Iter = HoudiniAssetObjectGeos.CreateIterator(); Iter; ++Iter)
	{
		FHoudiniAssetObjectGeo* HoudiniAssetObjectGeo = *Iter;
		HoudiniAssetObjectGeo->CollectTextures(HoudiniTextures);
	}
}


void
UHoudiniAssetComponent::CreateRenderingResources()
{
	for(TArray<FHoudiniAssetObjectGeo*>::TIterator Iter = HoudiniAssetObjectGeos.CreateIterator(); Iter; ++Iter)
	{
		FHoudiniAssetObjectGeo* HoudiniAssetObjectGeo = *Iter;
		HoudiniAssetObjectGeo->CreateRenderingResources();
	}
}


void
UHoudiniAssetComponent::ReleaseRenderingResources()
{
	if(HoudiniAssetObjectGeos.Num() > 0)
	{
		for(TArray<FHoudiniAssetObjectGeo*>::TIterator Iter = HoudiniAssetObjectGeos.CreateIterator(); Iter; ++Iter)
		{
			FHoudiniAssetObjectGeo* HoudiniAssetObjectGeo = *Iter;
			HoudiniAssetObjectGeo->ReleaseRenderingResources();
		}

		// Insert a fence to signal when these commands completed.
		ReleaseResourcesFence.BeginFence();
		bAsyncResourceReleaseHasBeenStarted = true;

		// Wait for fence to complete.
		ReleaseResourcesFence.Wait();
	}

	bAsyncResourceReleaseHasBeenStarted = false;
}


void
UHoudiniAssetComponent::UpdateRenderingInformation()
{
	// Need to send this to render thread at some point.
	MarkRenderStateDirty();

	// Update physics representation right away.
	RecreatePhysicsState();

	// Since we have new asset, we need to update bounds.
	UpdateBounds();
}


FPrimitiveSceneProxy*
UHoudiniAssetComponent::CreateSceneProxy()
{
	FPrimitiveSceneProxy* Proxy = nullptr;

	if(ContainsGeos())
	{
		Proxy = new FHoudiniMeshSceneProxy(this);
	}

	return Proxy;
}


void
UHoudiniAssetComponent::OnComponentDestroyed()
{
	HOUDINI_LOG_MESSAGE(TEXT("Destroying component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);

	if(HapiGUID.IsValid())
	{
		// If we have a valid task GUID.
		FHoudiniEngineTaskInfo TaskInfo;

		if(FHoudiniEngine::Get().RetrieveTaskInfo(HapiGUID, TaskInfo))
		{
			FHoudiniEngine::Get().RemoveTaskInfo(HapiGUID);
			HapiGUID.Invalidate();
			StopHoudiniTicking();

			if(NotificationPtr.IsValid())
			{
				TSharedPtr<SNotificationItem> NotificationItem = NotificationPtr.Pin();
				if(NotificationItem.IsValid())
				{
					NotificationItem->ExpireAndFadeout();
					NotificationPtr.Reset();
				}
			}
		}
	}

	// Clear collected textures.
	HoudiniTextures.Reset();

	// Before releasing resources make sure we do not have scene proxy active.
	//check(!SceneProxy);

	// Now we can release rendering resources.
	ReleaseRenderingResources();

	// Make sure fence release is complete.
	check(ReleaseResourcesFence.IsFenceComplete());

	// Release all geo and part objects.
	ClearGeos();

	// If we have an asset.
	if(-1 != AssetId)
	{
		// Generate GUID for our new task.
		HapiGUID = FGuid::NewGuid();

		// Create asset deletion task object and submit it for processing.
		FHoudiniEngineTask Task(EHoudiniEngineTaskType::AssetDeletion, HapiGUID);
		Task.AssetId = AssetId;
		FHoudiniEngine::Get().AddTask(Task);

		// Reset asset id.
		AssetId = -1;
	}

	// Unsubscribe from World save events.
	UnsubscribeSaveWorldDelegates();

	// Call super class implementation.
	Super::OnComponentDestroyed();
}


void
UHoudiniAssetComponent::BeginDestroy()
{
	// Notify that the primitive has been detached from this component.
	IStreamingManager::Get().NotifyPrimitiveDetached(this);

	Super::BeginDestroy();
}


void
UHoudiniAssetComponent::FinishDestroy()
{
	Super::FinishDestroy();
}


bool
UHoudiniAssetComponent::IsReadyForFinishDestroy()
{
	return Super::IsReadyForFinishDestroy() && ReleaseResourcesFence.IsFenceComplete();
}


void
UHoudiniAssetComponent::RemoveMetaDataFromEnum(UEnum* EnumObject)
{
	for(int Idx = 0; Idx < EnumObject->NumEnums(); ++Idx)
	{
		if(EnumObject->HasMetaData(TEXT("DisplayName"), Idx))
		{
			EnumObject->RemoveMetaData(TEXT("DisplayName"), Idx);
		}

		if(EnumObject->HasMetaData(TEXT("HoudiniName"), Idx))
		{
			EnumObject->RemoveMetaData(TEXT("HoudiniName"), Idx);
		}
	}
}


void
UHoudiniAssetComponent::ReplacePropertyOffset(UProperty* Property, int Offset)
{
	// We need this bit of magic in order to replace the private field.
	*(int*)((char*)&Property->RepNotifyFunc + sizeof(FName)) = Offset;
}


EHoudiniEngineProperty::Type 
UHoudiniAssetComponent::GetPropertyType(UProperty* Property) const
{
	if(UIntProperty::StaticClass() == Property->GetClass())
	{
		return EHoudiniEngineProperty::Integer;
	}
	else if(UBoolProperty::StaticClass() == Property->GetClass())
	{
		return EHoudiniEngineProperty::Boolean;
	}
	else if(UFloatProperty::StaticClass() == Property->GetClass())
	{
		return EHoudiniEngineProperty::Float;
	}
	else if(UStrProperty::StaticClass() == Property->GetClass())
	{
		return EHoudiniEngineProperty::String;
	}
	else if(UByteProperty::StaticClass() == Property->GetClass())
	{
		return EHoudiniEngineProperty::Enumeration;
	}
	else if(UStructProperty::StaticClass() == Property->GetClass())
	{
		UStructProperty* StructProperty = Cast<UStructProperty>(Property);

		if(UHoudiniAssetComponent::ScriptStructColor == StructProperty->Struct)
		{
			return EHoudiniEngineProperty::Color;
		}
	}

	return EHoudiniEngineProperty::None;
}


void
UHoudiniAssetComponent::SubscribeSaveWorldDelegates()
{
	FEditorDelegates::PreSaveWorld.AddUObject(this, &UHoudiniAssetComponent::OnPreSaveWorld);
	FEditorDelegates::PostSaveWorld.AddUObject(this, &UHoudiniAssetComponent::OnPostSaveWorld);
}


void
UHoudiniAssetComponent::UnsubscribeSaveWorldDelegates()
{
	FEditorDelegates::PreSaveWorld.RemoveUObject(this, &UHoudiniAssetComponent::OnPreSaveWorld);
	FEditorDelegates::PostSaveWorld.RemoveUObject(this, &UHoudiniAssetComponent::OnPostSaveWorld);
}


void
UHoudiniAssetComponent::ReplaceClassInformation(const FString& ActorLabel, bool bReplace)
{
	UClass* NewClass = nullptr;
	UClass* ClassOfUHoudiniAssetComponent = UHoudiniAssetComponent::StaticClass();

	// If RTTI has not been previously patched, we need to do so.
	if(!PatchedClass)
	{
		// Construct unique name for this class.
		FString PatchedClassName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s_%d"), *GetClass()->GetName(), *ActorLabel, UHoudiniAssetComponent::ComponentPatchedClassCounter));

		// Create new class instance.
		static const EObjectFlags PatchedClassFlags = RF_Public | RF_Standalone;

		// Construct the new class instance.
		NewClass = ConstructObject<UClass>(UClass::StaticClass(), GetOutermost(), FName(*PatchedClassName), PatchedClassFlags, ClassOfUHoudiniAssetComponent, true);

		// We just created a patched instance.
		UHoudiniAssetComponent::ComponentPatchedClassCounter++;

		// Use same class flags as the original class. Also make sure we remove intrinsic flag. Intrinsic flag specifies
		// that class has been generated in c++ and has no boilerplate generated by UnrealHeaderTool.
		NewClass->ClassFlags = UHoudiniAssetComponent::StaticClassFlags & ~CLASS_Intrinsic;

		// Use same class cast flags as the original class (these are used for quick casting between common types).
		NewClass->ClassCastFlags = UHoudiniAssetComponent::StaticClassCastFlags();

		// Use same class configuration name.
		NewClass->ClassConfigName = UHoudiniAssetComponent::StaticConfigName();

		// We will reuse the same constructor as nothing has really changed.
		NewClass->ClassConstructor = ClassOfUHoudiniAssetComponent->ClassConstructor;

		// Register our own reference counting registration.
		NewClass->ClassAddReferencedObjects = UHoudiniAssetComponent::AddReferencedObjects;

		// Minimum class alignment does not change.
		NewClass->MinAlignment = ClassOfUHoudiniAssetComponent->MinAlignment;

		// Properties size does not change as we use the same fixed size buffer.
		NewClass->PropertiesSize = ClassOfUHoudiniAssetComponent->PropertiesSize;

		// Set super class (we are deriving from UHoudiniAssetComponent).
		NewClass->SetSuperStruct(ClassOfUHoudiniAssetComponent->GetSuperStruct());

		// Create Class default object.
		NewClass->ClassDefaultObject = GetClass()->ClassDefaultObject;

		// List of replication records.
		NewClass->ClassReps = ClassOfUHoudiniAssetComponent->ClassReps;

		// List of network relevant fields (properties and functions).
		NewClass->NetFields = ClassOfUHoudiniAssetComponent->NetFields;

		// Reference token stream used by real time garbage collector.
		NewClass->ReferenceTokenStream = ClassOfUHoudiniAssetComponent->ReferenceTokenStream;

		// This class's native functions.
		NewClass->NativeFunctionLookupTable = ClassOfUHoudiniAssetComponent->NativeFunctionLookupTable;

		// Store patched class.
		PatchedClass = NewClass;

		// Now that we've filled all necessary fields, patch class information.
		if(bReplace)
		{
			ReplaceClassObject(NewClass);

			// Now that RTTI has been patched, we need to subscribe to World save delegates. This is necessary in order to
			// patch old RTTI information back for saving. Once save completes, we restore the patched RTTI back.
			SubscribeSaveWorldDelegates();
		}
	}
	else
	{
		// Otherwise we need to destroy and recreate all properties.
		NewClass = GetClass();
		RemoveClassProperties(NewClass);
	}

	// Insert necessary properties.
	ReplaceClassProperties(NewClass);
}


void
UHoudiniAssetComponent::ReplaceClassObject(UClass* ClassObjectNew)
{
	HOUDINI_PRIVATE_CALL(FObjectBaseAccess, UObjectBase, ClassObjectNew);
}


void
UHoudiniAssetComponent::RemoveClassProperties(UClass* ClassInstance)
{
	UClass* ClassOfUHoudiniAssetComponent = UHoudiniAssetComponent::StaticClass();

	UProperty* IterProperty = ClassInstance->PropertyLink;
	while(IterProperty && IterProperty != ClassOfUHoudiniAssetComponent->PropertyLink)
	{
		UProperty* Property = IterProperty;
		IterProperty = IterProperty->PropertyLinkNext;

		//Property->ClearFlags(RF_Native | RF_RootSet);
		Property->Next = nullptr;
		Property->PropertyLinkNext = nullptr;
	}

	// Do not need to update / remove / delete children as those will be by construction same as properties.
}


bool
UHoudiniAssetComponent::ReplaceClassProperties(UClass* ClassInstance)
{
	HAPI_Result Result = HAPI_RESULT_SUCCESS;
	HAPI_AssetInfo AssetInfo;
	HAPI_NodeInfo NodeInfo;

	std::vector<HAPI_ParmInfo> ParmInfo;
	std::vector<int> ParmValuesIntegers;
	std::vector<float> ParmValuesFloats;
	std::vector<HAPI_StringHandle> ParmValuesStrings;
	std::vector<char> ParmName;
	std::vector<char> ParmLabel;

	if(-1 == AssetId)
	{
		// There's no Houdini asset, we can return. This is typically hit when component is being loaded during serialization.
		return true;
	}

	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetAssetInfo(AssetId, &AssetInfo), false);
	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetNodeInfo(AssetInfo.nodeId, &NodeInfo), false);

	// Retrieve parameters.
	ParmInfo.resize(NodeInfo.parmCount);
	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParameters(AssetInfo.nodeId, &ParmInfo[0], 0, NodeInfo.parmCount), false);

	// Retrieve integer values for this asset.
	ParmValuesIntegers.resize(NodeInfo.parmIntValueCount);
	if(NodeInfo.parmIntValueCount > 0)
	{
		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmIntValues(AssetInfo.nodeId, &ParmValuesIntegers[0], 0, NodeInfo.parmIntValueCount), false);
	}

	// Retrieve float values for this asset.
	ParmValuesFloats.resize(NodeInfo.parmFloatValueCount);
	if(NodeInfo.parmFloatValueCount > 0)
	{
		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmFloatValues(AssetInfo.nodeId, &ParmValuesFloats[0], 0, NodeInfo.parmFloatValueCount), false);
	}

	// Retrieve string values for this asset.
	ParmValuesStrings.resize(NodeInfo.parmStringValueCount);
	if(NodeInfo.parmStringValueCount > 0)
	{
		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmStringValues(AssetInfo.nodeId, true, &ParmValuesStrings[0], 0, NodeInfo.parmStringValueCount), false);
	}

	// Reset list which keeps track of properties we have created.
	CreatedProperties.Reset();

	// We need to insert new properties and new children in the beginning of single link list.
	// This way properties and children from the original class can be reused and will not have
	// their next pointers altered.
	UProperty* PropertyFirst = nullptr;
	UProperty* PropertyLast = PropertyFirst;

	UField* ChildFirst = nullptr;
	UField* ChildLast = ChildFirst;

	uint32 ValuesOffsetStart = offsetof(UHoudiniAssetComponent, ScratchSpaceBuffer);
	uint32 ValuesOffsetEnd = ValuesOffsetStart;

	for(int idx = 0; idx < NodeInfo.parmCount; ++idx)
	{
		// Retrieve param info at this index.
		const HAPI_ParmInfo& ParmInfoIter = ParmInfo[idx];

		// If parameter is invisible, skip it.
		if(ParmInfoIter.invisible)
		{
			continue;
		}

		// Skip unsupported param types for now.
		switch(ParmInfoIter.type)
		{
			case HAPI_PARMTYPE_INT:
			case HAPI_PARMTYPE_FLOAT:
			case HAPI_PARMTYPE_TOGGLE:
			case HAPI_PARMTYPE_COLOR:
			case HAPI_PARMTYPE_STRING:
			{
				break;
			}

			default:
			{
				// Just ignore unsupported types for now.
				continue;
			}
		}

		// Retrieve length of this parameter's name.
		int32 ParmNameLength = 0;
		HOUDINI_CHECK_ERROR(&Result, HAPI_GetStringBufLength(ParmInfoIter.nameSH, &ParmNameLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving length of this parameter's name, continue onto next parameter.
			continue;
		}

		// If length of name of this parameter is zero, continue onto next parameter.
		if(!ParmNameLength)
		{
			continue;
		}

		// Retrieve name for this parameter.
		ParmName.resize(ParmNameLength);
		HOUDINI_CHECK_ERROR(&Result, HAPI_GetString(ParmInfoIter.nameSH, &ParmName[0], ParmNameLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving the name of this parameter, continue onto next parameter.
			continue;
		}

		// We need to convert name to a string Unreal understands.
		FUTF8ToTCHAR ParamNameStringConverter(&ParmName[0]);
		FName ParmNameConverted = ParamNameStringConverter.Get();

		// Create unique property name to avoid collisions.
		FString UniquePropertyName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s"), *ClassInstance->GetName(), *ParmNameConverted.ToString()));

		// Retrieve length of this parameter's label.
		int32 ParmLabelLength = 0;
		HOUDINI_CHECK_ERROR(&Result, HAPI_GetStringBufLength(ParmInfoIter.labelSH, &ParmLabelLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving length of this parameter's label, continue onto next parameter.
			continue;
		}

		// Retrieve label for this parameter.
		ParmLabel.resize(ParmLabelLength);
		HOUDINI_CHECK_ERROR(&Result, HAPI_GetString(ParmInfoIter.labelSH, &ParmLabel[0], ParmLabelLength));
		if(HAPI_RESULT_SUCCESS != Result)
		{
			// We have encountered an error retrieving the label of this parameter, continue onto next parameter.
			continue;
		}

		// We need to convert label to a string Unreal understands.
		FUTF8ToTCHAR ParamLabelStringConverter(&ParmLabel[0]);

		UProperty* Property = nullptr;

		switch(ParmInfoIter.type)
		{
			case HAPI_PARMTYPE_INT:
			{
				if(!ParmInfoIter.choiceCount)
				{
					Property = CreatePropertyInt(ClassInstance, UniquePropertyName, ParmInfoIter.size, &ParmValuesIntegers[ParmInfoIter.intValuesIndex], ValuesOffsetEnd);
				}
				else if(ParmInfoIter.choiceIndex >= 0)
				{
					// This parameter is an integer choice list.

					// Get relevant choices.
					std::vector<HAPI_ParmChoiceInfo> ChoiceInfos;
					ChoiceInfos.resize(ParmInfoIter.choiceCount);
					HOUDINI_CHECK_ERROR(&Result, HAPI_GetParmChoiceLists(NodeInfo.id, &ChoiceInfos[0], ParmInfoIter.choiceIndex, ParmInfoIter.choiceCount));
					if(HAPI_RESULT_SUCCESS != Result)
					{
						continue;
					}

					// Retrieve enum value from HAPI.
					int EnumIndex = 0;
					HOUDINI_CHECK_ERROR(&Result, HAPI_GetParmIntValues(NodeInfo.id, &EnumIndex, ParmInfoIter.intValuesIndex, ParmInfoIter.size));

					// Create enum property.
					Property = CreatePropertyEnum(ClassInstance, UniquePropertyName, ChoiceInfos, EnumIndex, ValuesOffsetEnd);
				}

				break;
			}

			case HAPI_PARMTYPE_STRING:
			{
				if(!ParmInfoIter.choiceCount)
				{
					Property = CreatePropertyString(ClassInstance, UniquePropertyName, ParmInfoIter.size, &ParmValuesStrings[ParmInfoIter.stringValuesIndex], ValuesOffsetEnd);
				}
				else if(ParmInfoIter.choiceIndex >= 0)
				{
					// This parameter is a string choice list.

					// Get relevant choices.
					std::vector<HAPI_ParmChoiceInfo> ChoiceInfos;
					ChoiceInfos.resize(ParmInfoIter.choiceCount);
					HOUDINI_CHECK_ERROR(&Result, HAPI_GetParmChoiceLists(NodeInfo.id, &ChoiceInfos[0], ParmInfoIter.choiceIndex, ParmInfoIter.choiceCount));
					if(HAPI_RESULT_SUCCESS != Result)
					{
						continue;
					}
					
					// Retrieve enum value from HAPI.
					int EnumValue = 0;
					HOUDINI_CHECK_ERROR(&Result, HAPI_GetParmStringValues(NodeInfo.id, false, &EnumValue, ParmInfoIter.stringValuesIndex, ParmInfoIter.size));

					// Retrieve string value.
					FString EnumStringValue;
					if(!FHoudiniEngineUtils::GetHoudiniString(EnumValue, EnumStringValue))
					{
						continue;
					}

					// Create enum property.
					Property = CreatePropertyEnum(ClassInstance, UniquePropertyName, ChoiceInfos, EnumStringValue, ValuesOffsetEnd);
				}

				break;
			}

			case HAPI_PARMTYPE_FLOAT:
			{
				Property = CreatePropertyFloat(ClassInstance, UniquePropertyName, ParmInfoIter.size, &ParmValuesFloats[ParmInfoIter.floatValuesIndex], ValuesOffsetEnd);
				break;
			}

			case HAPI_PARMTYPE_TOGGLE:
			{
				Property = CreatePropertyToggle(ClassInstance, UniquePropertyName, ParmInfoIter.size, &ParmValuesIntegers[ParmInfoIter.intValuesIndex], ValuesOffsetEnd);
				break;
			}

			case HAPI_PARMTYPE_COLOR:
			{
				Property = CreatePropertyColor(ClassInstance, UniquePropertyName, ParmInfoIter.size, &ParmValuesFloats[ParmInfoIter.floatValuesIndex], ValuesOffsetEnd);
				break;
			}

			default:
			{
				continue;
			}
		}

		if(!Property)
		{
			// Unsupported type property - skip to next parameter.
			continue;
		}

		// Store parameter name as meta data.
		Property->SetMetaData(TEXT("HoudiniParmName"), ParamNameStringConverter.Get());

		// Use label instead of name if it is present.
		if(ParmLabelLength)
		{
			Property->SetMetaData(TEXT("DisplayName"), ParamLabelStringConverter.Get());
		}
		else
		{
			Property->SetMetaData(TEXT("DisplayName"), ParamNameStringConverter.Get());
		}

		// Set UI and physical ranges, if present.
		if(ParmInfoIter.hasUIMin)
		{
			Property->SetMetaData(TEXT("UIMin"), *FString::SanitizeFloat(ParmInfoIter.UIMin));
		}

		if(ParmInfoIter.hasUIMax)
		{
			Property->SetMetaData(TEXT("UIMax"), *FString::SanitizeFloat(ParmInfoIter.UIMax));
		}

		if(ParmInfoIter.hasMin)
		{
			Property->SetMetaData(TEXT("ClampMin"), *FString::SanitizeFloat(ParmInfoIter.min));
		}

		if(ParmInfoIter.hasMax)
		{
			Property->SetMetaData(TEXT("ClampMax"), *FString::SanitizeFloat(ParmInfoIter.max));
		}

		// Store this property in a list of created properties.
		CreatedProperties.Add(Property);

		// Insert this newly created property in link list of properties.
		if(!PropertyFirst)
		{
			PropertyFirst = Property;
			PropertyLast = Property;
		}
		else
		{
			PropertyLast->PropertyLinkNext = Property;
			PropertyLast = Property;
		}

		// Insert this newly created property into link list of children.
		if(!ChildFirst)
		{
			ChildFirst = Property;
			ChildLast = Property;
		}
		else
		{
			ChildLast->Next = Property;
			ChildLast = Property;
		}
	}

	UClass* ClassOfUHoudiniAssetComponent = UHoudiniAssetComponent::StaticClass();

	if(PropertyFirst)
	{
		ClassInstance->PropertyLink = PropertyFirst;
		PropertyLast->PropertyLinkNext = ClassOfUHoudiniAssetComponent->PropertyLink;
	}

	if(ChildFirst)
	{
		ClassInstance->Children = ChildFirst;
		ChildLast->Next = ClassOfUHoudiniAssetComponent->Children;
	}

	return true;
}


UField*
UHoudiniAssetComponent::CreateEnum(UClass* ClassInstance, const FString& Name, const std::vector<HAPI_ParmChoiceInfo>& Choices)
{
	// Create label for this enum.
	FString UniqueEnumName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("enum_%s"), *Name));

	// See if enum has already been created.
	UEnum* ChoiceEnum = FindObject<UEnum>(ClassInstance, *UniqueEnumName, false);
	if(!ChoiceEnum)
	{
		// Enum does not exist, we need to create a corresponding enum.
		static const EObjectFlags EnumObjectFlags = RF_Public | RF_Transient;
		ChoiceEnum = NewNamedObject<UEnum>(ClassInstance, FName(*UniqueEnumName), EnumObjectFlags);
	}

	// Remove all previous meta data.
	RemoveMetaDataFromEnum(ChoiceEnum);

	FString EnumValueName, EnumFinalValue;
	TArray<FName> EnumValues;
	TArray<FString> EnumValueNames;
	TArray<FString> EnumValueLabels;

	// Retrieve string values for these parameters.
	for(int ChoiceIdx = 0; ChoiceIdx < Choices.size(); ++ChoiceIdx)
	{
		// Process labels.
		if(FHoudiniEngineUtils::GetHoudiniString(Choices[ChoiceIdx].labelSH, EnumValueName))
		{
			EnumValueLabels.Add(EnumValueName);

			EnumFinalValue = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_value_%s"), *UniqueEnumName, *EnumValueName));
			EnumValues.Add(FName(*EnumFinalValue));
		}
		else
		{
			break;
		}

		// Process names.
		if(FHoudiniEngineUtils::GetHoudiniString(Choices[ChoiceIdx].valueSH, EnumValueName))
		{
			EnumValueNames.Add(EnumValueName);
		}
		else
		{
			break;
		}
	}

	// Make sure strings have been properly retrieved.
	if(EnumValues.Num() != Choices.size())
	{
		ChoiceEnum->MarkPendingKill();
		ChoiceEnum = nullptr;
	}
	else
	{
		// Set enum entries (this will also remove previous entries).
		ChoiceEnum->SetEnums(EnumValues, false);

		// We need to set meta data in a separate pass (as meta data requires enum being initialized).
		for(int ChoiceIdx = 0; ChoiceIdx < Choices.size(); ++ChoiceIdx)
		{
			ChoiceEnum->SetMetaData(TEXT("DisplayName"), *(EnumValueLabels[ChoiceIdx]), ChoiceIdx);
			ChoiceEnum->SetMetaData(TEXT("HoudiniName"), *(EnumValueNames[ChoiceIdx]), ChoiceIdx);
		}
	}

	return ChoiceEnum;
}


UProperty*
UHoudiniAssetComponent::CreateProperty(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags, EHoudiniEngineProperty::Type PropertyType)
{
	UProperty* Property = nullptr;

	switch(PropertyType)
	{
		case EHoudiniEngineProperty::Float:
		{
			Property = CreatePropertyFloat(ClassInstance, Name, PropertyFlags);
			break;
		}

		case EHoudiniEngineProperty::Integer:
		{
			Property = CreatePropertyInt(ClassInstance, Name, PropertyFlags);
			break;
		}

		case EHoudiniEngineProperty::Boolean:
		{
			Property = CreatePropertyToggle(ClassInstance, Name, PropertyFlags);
			break;
		}

		case EHoudiniEngineProperty::String:
		{
			Property = CreatePropertyString(ClassInstance, Name, PropertyFlags);
			break;
		}

		case EHoudiniEngineProperty::Color:
		{
			Property = CreatePropertyColor(ClassInstance, Name, PropertyFlags);
			break;
		}

		case EHoudiniEngineProperty::Enumeration:
		{
			//Property = CreatePropertyEnum(ClassInstance, Name, PropertyFlags);
			break;
		}

		default:
		{
			break;
		}
	}

	if(Property)
	{
		CreatedProperties.Add(Property);
	}

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyEnum(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient;

	UByteProperty* Property = FindObject<UByteProperty>(ClassInstance, *Name, false);
	if(!Property)
	{
		Property = NewNamedObject<UByteProperty>(ClassInstance, FName(*Name), PropertyObjectFlags);
	}

	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniProperties"));
	Property->PropertyFlags = PropertyFlags;

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyEnum(UClass* ClassInstance, const FString& Name, const std::vector<HAPI_ParmChoiceInfo>& Choices, int32 Value, uint32& Offset)
{
	static const uint64 PropertyFlags = UINT64_C(69793219077);

	// We need to create or reuse an enum for this property.
	UEnum* EnumType = Cast<UEnum>(CreateEnum(ClassInstance, Name, Choices));
	if(!EnumType)
	{
		return nullptr;
	}

	UByteProperty* Property = Cast<UByteProperty>(CreatePropertyEnum(ClassInstance, Name, PropertyFlags));

	// Set the enum for this property.
	Property->Enum = EnumType;

	// Set property size. Larger than one indicates array.
	Property->ArrayDim = 1;

	// Enum uses unsigned byte.
	uint8* Boundary = ComputeOffsetAlignmentBoundary<uint8>(Offset);
	Offset = (const char*) Boundary - (const char*) this;

	// Need to patch offset for this property.
	ReplacePropertyOffset(Property, Offset);

	// Write property data to which it refers by offset.
	*Boundary = (uint8) Value;

	// Increment offset for next property.
	Offset = Offset + sizeof(uint8);

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyEnum(UClass* ClassInstance, const FString& Name, const std::vector<HAPI_ParmChoiceInfo>& Choices, const FString& ValueString, uint32& Offset)
{
	// Store initial offset.
	uint32 OffsetStored = Offset;

	// Create enum property with default 0 value.
	UByteProperty* Property = Cast<UByteProperty>(CreatePropertyEnum(ClassInstance, Name, Choices, 0, Offset));
	if(Property)
	{
		// Get enum for this property.
		UEnum* Enum = Property->Enum;

		// Sanitize name for comparison.
		FString ValueStringCompare = ObjectTools::SanitizeObjectName(ValueString);

		// Empty string means index 0 (comes from Houdini) and we created property with 0 index by default.
		if(!ValueString.IsEmpty())
		{
			for(int Idx = 0; Idx < Enum->NumEnums(); ++Idx)
			{
				if(Enum->HasMetaData(TEXT("HoudiniName"), Idx))
				{
					const FString& HoudiniName = Enum->GetMetaData(TEXT("HoudiniName"), Idx);

					if(!HoudiniName.Compare(ValueStringCompare, ESearchCase::IgnoreCase))
					{
						// We need to repatch the value.
						uint8* Boundary = ComputeOffsetAlignmentBoundary<uint8>(OffsetStored);
						OffsetStored = (const char*)Boundary - (const char*) this;

						// Need to patch offset for this property.
						ReplacePropertyOffset(Property, OffsetStored);

						// Write property data to which it refers by offset.
						*Boundary = (uint8) Idx;

						// Increment offset for next property.
						Offset = OffsetStored + sizeof(uint8);

						break;
					}
				}
			}
		}

		// We will use meta information to mark this property as one corresponding to a string choice list.
		Property->SetMetaData(TEXT("HoudiniStringChoiceList"), TEXT("1"));
	}

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyString(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient;

	UStrProperty* Property = FindObject<UStrProperty>(ClassInstance, *Name, false);
	if(!Property)
	{
		// Property does not exist, we need to create it.
		Property = NewNamedObject<UStrProperty>(ClassInstance, FName(*Name), PropertyObjectFlags);
	}

	Property->PropertyFlags = PropertyFlags;
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniProperties"));

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyString(UClass* ClassInstance, const FString& Name, int Count, const HAPI_StringHandle* Value, uint32& Offset)
{
	static const uint64 PropertyFlags = UINT64_C(69793219077);

	// Ignore parameters with size zero.
	if(!Count)
	{
		return nullptr;
	}

	// Create property or locate existing.
	UProperty* Property = CreatePropertyString(ClassInstance, Name, PropertyFlags);

	// Set property size. Larger than one indicates array.
	Property->ArrayDim = Count;

	// We need to compute proper alignment for this type.
	FString* Boundary = ComputeOffsetAlignmentBoundary<FString>(Offset);
	Offset = (const char*) Boundary - (const char*) this;

	// Need to patch offset for this property.
	ReplacePropertyOffset(Property, Offset);

	// Write property data to which it refers by offset.
	for(int Index = 0; Index < Count; ++Index)
	{
		FString NameString;

		if(FHoudiniEngineUtils::GetHoudiniString(*(Value + Index), NameString))
		{
			new(Boundary) FString(NameString);
		}
		else
		{
			new(Boundary) FString(TEXT("Invalid"));
		}

		Offset += sizeof(FString);
	}

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyColor(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient;

	if(!UHoudiniAssetComponent::ScriptStructColor)
	{
		UHoudiniAssetComponent::ScriptStructColor = new(UHoudiniAssetComponent::StaticClass(), TEXT("Color"), RF_Public | RF_Transient | RF_Native) 
										UScriptStruct(FPostConstructInitializeProperties(), NULL, NULL, EStructFlags(0x00000030), sizeof(FColor), ALIGNOF(FColor));

		UProperty* NewProp_A = new(UHoudiniAssetComponent::ScriptStructColor, TEXT("A"), RF_Public | RF_Transient | RF_Native) UByteProperty(CPP_PROPERTY_BASE(A, FColor), 0x0000000001000005);
		UProperty* NewProp_R = new(UHoudiniAssetComponent::ScriptStructColor, TEXT("R"), RF_Public | RF_Transient | RF_Native) UByteProperty(CPP_PROPERTY_BASE(R, FColor), 0x0000000001000005);
		UProperty* NewProp_G = new(UHoudiniAssetComponent::ScriptStructColor, TEXT("G"), RF_Public | RF_Transient | RF_Native) UByteProperty(CPP_PROPERTY_BASE(G, FColor), 0x0000000001000005);
		UProperty* NewProp_B = new(UHoudiniAssetComponent::ScriptStructColor, TEXT("B"), RF_Public | RF_Transient | RF_Native) UByteProperty(CPP_PROPERTY_BASE(B, FColor), 0x0000000001000005);

		UHoudiniAssetComponent::ScriptStructColor->StaticLink();
	}

	UStructProperty* Property = FindObject<UStructProperty>(ClassInstance, *Name, false);
	if(!Property)
	{
		Property = NewNamedObject<UStructProperty>(ClassInstance, FName(*Name), PropertyObjectFlags);
		Property->Struct = UHoudiniAssetComponent::ScriptStructColor;
	}

	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniProperties"));
	Property->PropertyFlags = PropertyFlags;

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyColor(UClass* ClassInstance, const FString& Name, int Count, const float* Value, uint32& Offset)
{
	static const uint64 PropertyFlags = UINT64_C(69793219077);

	// Color must have 3 or 4 fields.
	if(Count < 3)
	{
		return nullptr;
	}

	UProperty* Property = CreatePropertyColor(ClassInstance, Name, PropertyFlags);

	FColor ConvertedColor;

	if(Count < 4)
	{
		// Disable alpha channel if our color does not have it.
		Property->SetMetaData(TEXT("HideAlphaChannel"), TEXT("0"));

		// Convert Houdini float RGB color to Unreal int RGB color (this will set alpha to 255).
		FHoudiniEngineUtils::ConvertHoudiniColorRGB(Value, ConvertedColor);
	}
	else
	{
		// Convert Houdini float RGBA color to Unreal int RGBA color.
		FHoudiniEngineUtils::ConvertHoudiniColorRGBA(Value, ConvertedColor);
	}

	// We need to compute proper alignment for this type.
	FColor* Boundary = ComputeOffsetAlignmentBoundary<FColor>(Offset);
	Offset = (const char*) Boundary - (const char*) this;

	// Need to patch offset for this property.
	ReplacePropertyOffset(Property, Offset);
	
	// Write property data to which it refers by offset.
	*Boundary = ConvertedColor;
	
	// Increment offset for next property.
	Offset = Offset + sizeof(FColor);

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyInt(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient;

	UIntProperty* Property = FindObject<UIntProperty>(ClassInstance, *Name, false);
	if(!Property)
	{
		// Property does not exist, we need to create it.
		Property = NewNamedObject<UIntProperty>(ClassInstance, FName(*Name), PropertyObjectFlags);
	}

	Property->PropertyFlags = PropertyFlags;
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniProperties"));

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyInt(UClass* ClassInstance, const FString& Name, int Count, const int32* Value, uint32& Offset)
{
	static const uint64 PropertyFlags =  UINT64_C(69793219077);

	// Ignore parameters with size zero.
	if(!Count)
	{
		return nullptr;
	}

	// Create property or locate existing.
	UProperty* Property = CreatePropertyInt(ClassInstance, Name, PropertyFlags);

	// Set property size. Larger than one indicates array.
	Property->ArrayDim = Count;

	// We need to compute proper alignment for this type.
	int* Boundary = ComputeOffsetAlignmentBoundary<int>(Offset);
	Offset = (const char*) Boundary - (const char*) this;

	// Need to patch offset for this property.
	ReplacePropertyOffset(Property, Offset);

	// Write property data to which it refers by offset.
	for(int Index = 0; Index < Count; ++Index)
	{
		*Boundary = *(Value + Index);
	}

	// Increment offset for next property.
	Offset = Offset + sizeof(int) * Count;

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyFloat(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient;

	UFloatProperty* Property = FindObject<UFloatProperty>(ClassInstance, *Name, false);
	if(!Property)
	{
		// Property does not exist, we need to create it.
		Property = NewNamedObject<UFloatProperty>(ClassInstance, FName(*Name), PropertyObjectFlags);
	}

	Property->PropertyFlags = PropertyFlags;
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniProperties"));

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyFloat(UClass* ClassInstance, const FString& Name, int Count, const float* Value, uint32& Offset)
{
	static const uint64 PropertyFlags =  UINT64_C(69793219077);

	// Ignore parameters with size zero.
	if(!Count)
	{
		return nullptr;
	}

	// Create property or locate existing.
	UProperty* Property = CreatePropertyFloat(ClassInstance, Name, PropertyFlags);

	// Set property size. Larger than one indicates array.
	Property->ArrayDim = Count;

	// We need to compute proper alignment for this type.
	float* Boundary = ComputeOffsetAlignmentBoundary<float>(Offset);
	Offset = (const char*) Boundary - (const char*) this;

	// Need to patch offset for this property.
	ReplacePropertyOffset(Property, Offset);

	// Write property data to which it refers by offset.
	for(int Index = 0; Index < Count; ++Index)
	{
		*Boundary = *(Value + Index);
	}

	// Increment offset for next property.
	Offset = Offset + sizeof(float) * Count;

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyToggle(UClass* ClassInstance, const FString& Name, uint64 PropertyFlags)
{
	static const EObjectFlags PropertyObjectFlags = RF_Public | RF_Transient;

	UBoolProperty* Property = FindObject<UBoolProperty>(ClassInstance, *Name, false);
	if(!Property)
	{
		// Property does not exist, we need to create it.
		Property = NewNamedObject<UBoolProperty>(ClassInstance, FName(*Name), PropertyObjectFlags);
	}

	Property->SetBoolSize(sizeof(bool), true);
	Property->PropertyFlags = PropertyFlags;
	Property->PropertyLinkNext = nullptr;
	Property->SetMetaData(TEXT("Category"), TEXT("HoudiniProperties"));

	return Property;
}


UProperty*
UHoudiniAssetComponent::CreatePropertyToggle(UClass* ClassInstance, const FString& Name, int Count, const int32* bValue, uint32& Offset)
{
	static const uint64 PropertyFlags = UINT64_C(69793219077);

	// Ignore parameters with size zero.
	if(!Count)
	{
		return nullptr;
	}

	// Create property or locate existing.
	UProperty* Property = CreatePropertyToggle(ClassInstance, Name, PropertyFlags);

	// Set property size. Larger than one indicates array.
	Property->ArrayDim = Count;

	// We need to compute proper alignment for this type.
	bool* Boundary = ComputeOffsetAlignmentBoundary<bool>(Offset);
	Offset = (const char*) Boundary - (const char*) this;

	// Need to patch offset for this property.
	ReplacePropertyOffset(Property, Offset);

	// Write property data to which it refers by offset.
	for(int Index = 0; Index < Count; ++Index)
	{
		*Boundary = (*(bValue + Index) != 0);
	}

	// Increment offset for next property.
	Offset = Offset + sizeof(bool) * Count;

	return Property;
}


void
UHoudiniAssetComponent::SetChangedParameterValues()
{
	HAPI_Result Result = HAPI_RESULT_SUCCESS;
	HAPI_AssetInfo AssetInfo;

	HOUDINI_CHECK_ERROR_RETURN(HAPI_GetAssetInfo(AssetId, &AssetInfo), void());

	for(TSet<UProperty*>::TIterator Iter(ChangedProperties); Iter; ++Iter)
	{
		UProperty* Property = *Iter;

		// Retrieve offset into scratch space for this property.
		uint32 ValueOffset = Property->GetOffset_ForDebug();

		// Retrieve parameter name.
		const FString& ParameterName = Property->GetMetaData(TEXT("HoudiniParmName"));
		std::wstring PropertyName(*ParameterName);
		std::string PropertyNameConverted(PropertyName.begin(), PropertyName.end());

		HAPI_ParmId ParamId;
		HAPI_ParmInfo ParamInfo;

		HOUDINI_CHECK_ERROR_RETURN(HAPI_GetParmIdFromName(AssetInfo.nodeId, PropertyNameConverted.c_str(), &ParamId), void());
		HOUDINI_CHECK_ERROR(&Result, HAPI_GetParameters(AssetInfo.nodeId, &ParamInfo, ParamId, 1));

		if(-1 == ParamId)
		{
			// Parameter has not been found, skip this property.
			continue;
		}

		if(UIntProperty::StaticClass() == Property->GetClass())
		{
			check(ParamInfo.size == Property->ArrayDim);

			std::vector<int> Values(Property->ArrayDim, 0);
			for(int Index = 0; Index < Property->ArrayDim; ++Index)
			{
				Values[Index] = *(int*)((const char*) this + ValueOffset);
				ValueOffset += sizeof(int);
			}

			HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmIntValues(AssetInfo.nodeId, &Values[0], ParamInfo.intValuesIndex, ParamInfo.size));
		}
		else if(UBoolProperty::StaticClass() == Property->GetClass())
		{
			check(ParamInfo.size == Property->ArrayDim);

			std::vector<int> Values(Property->ArrayDim, 0);
			for(int Index = 0; Index < Property->ArrayDim; ++Index)
			{
				Values[Index] = *(bool*)((const char*) this + ValueOffset);
				ValueOffset += sizeof(bool);
			}

			HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmIntValues(AssetInfo.nodeId, &Values[0], ParamInfo.intValuesIndex, ParamInfo.size));
		}
		else if(UFloatProperty::StaticClass() == Property->GetClass())
		{
			check(ParamInfo.size == Property->ArrayDim);

			std::vector<float> Values(Property->ArrayDim, 0.0f);
			for(int Index = 0; Index < Property->ArrayDim; ++Index)
			{
				Values[Index] = *(float*)((const char*) this + ValueOffset);
				ValueOffset += sizeof(float);
			}

			HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmFloatValues(AssetInfo.nodeId, &Values[0], ParamInfo.floatValuesIndex, ParamInfo.size));
		}
		else if(UStrProperty::StaticClass() == Property->GetClass())
		{
			check(ParamInfo.size == Property->ArrayDim);

			for(int Index = 0; Index < Property->ArrayDim; ++Index)
			{
				// Get string at this index.
				FString* UnrealString = (FString*) ((const char*) this + ValueOffset);
				std::string String = TCHAR_TO_ANSI(*(*UnrealString));

				HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmStringValue(AssetInfo.nodeId, String.c_str(), ParamId, Index));

				// Continue onto next offset.
				ValueOffset += sizeof(FString);
			}
		}
		else if(UByteProperty::StaticClass() == Property->GetClass())
		{
			UByteProperty* ByteProperty = Cast<UByteProperty>(Property);

			// Get index value at this offset.
			int EnumValue = (int)(*(uint8*)((const char*) this + ValueOffset));

			if(ByteProperty->HasMetaData(TEXT("HoudiniStringChoiceList")))
			{
				// This property corresponds to a string choice list.
				FText EnumText = ByteProperty->Enum->GetEnumText(EnumValue);
				std::string String = TCHAR_TO_ANSI(*EnumText.ToString());

				HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmStringValue(AssetInfo.nodeId, String.c_str(), ParamId, 0));
			}
			else
			{
				// This property corresponds to an integer choice list.
				HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmIntValues(AssetInfo.nodeId, &EnumValue, ParamInfo.intValuesIndex, ParamInfo.size));
			}
		}
		else if(UStructProperty::StaticClass() == Property->GetClass())
		{
			UStructProperty* StructProperty = Cast<UStructProperty>(Property);

			if(UHoudiniAssetComponent::ScriptStructColor == StructProperty->Struct)
			{
				// Extract color information.
				FColor UnrealColor = *(FColor*)((const char*) this + ValueOffset);
				std::vector<float> Values(4, 0.0f);

				if(StructProperty->HasMetaData(TEXT("HideAlphaChannel")))
				{
					FHoudiniEngineUtils::ConvertUnrealColorRGB(UnrealColor, &Values[0]);
					Values[3] = 1.0f;
				}
				else
				{
					FHoudiniEngineUtils::ConvertUnrealColorRGBA(UnrealColor, &Values[0]);
				}

				HOUDINI_CHECK_ERROR(&Result, HAPI_SetParmFloatValues(AssetInfo.nodeId, &Values[0], ParamInfo.floatValuesIndex, ParamInfo.size));
			}
		}
	}
}


void
UHoudiniAssetComponent::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	// Retrieve property which changed. Property field is a property which is being modified. MemberProperty
	// field is a property which contains the modified property (for example if modified property is a member of a
	// struct).
	UProperty* Property = PropertyChangedEvent.MemberProperty;
	UProperty* PropertyChild = PropertyChangedEvent.Property;

	// Retrieve property category.
	static const FString CategoryHoudiniAsset = TEXT("HoudiniProperties");
	const FString& Category = Property->GetMetaData(TEXT("Category"));

	if(Category != CategoryHoudiniAsset)
	{
		// This property is not in category we are interested in, just jump out.
		return;
	}

	HOUDINI_LOG_MESSAGE(TEXT("PostEditChangeProperty, Property = 0x%0.8p, PropertyChild = 0x%0.8p"), Property, PropertyChild);

	if(EPropertyChangeType::Interactive == PropertyChangedEvent.ChangeType)
	{
		if(UStructProperty::StaticClass() == Property->GetClass())
		{
			UStructProperty* StructProperty = Cast<UStructProperty>(Property);

			if(UHoudiniAssetComponent::ScriptStructColor == StructProperty->Struct)
			{
				// Ignore interactive events for color properties.
				return;
			}
		}
	}

	// If this is a loaded component, we need instantiation.
	if(bLoadedComponent && (-1 == AssetId) && !bLoadedComponentRequiresInstantiation)
	{
		bLoadedComponentRequiresInstantiation = true;
	}

	// Mark this property as changed.
	Property->SetMetaData(TEXT("HoudiniPropertyChanged"), TEXT("1"));

	// Add changed property to the set of changes.
	ChangedProperties.Add(Property);

	// Start ticking (if we are ticking already, this will be ignored).
	StartHoudiniTicking();
}


void
UHoudiniAssetComponent::OnRegister()
{
	Super::OnRegister();
	HOUDINI_LOG_MESSAGE(TEXT("Registering component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);

	// Make sure we have a Houdini asset to operate with.
	if(!HoudiniAsset)
	{
		return;
	}

	TWeakObjectPtr<AHoudiniAssetActor> HoudiniAssetActor = GetHoudiniAssetActorOwner();
	if(!HoudiniAssetActor.IsValid())
	{
		return;
	}

	if(bIsNativeComponent)
	{
		// This is a native component ~ belonging to a c++ actor.

		if(bIsPreviewComponent)
		{
			HOUDINI_LOG_MESSAGE(TEXT("Native::OnRegister, Preview actor"));
		}
		else
		{
			HOUDINI_LOG_MESSAGE(TEXT("Native::OnRegister, Non-preview actor"));
		}
	}
	else
	{
		// This is a dynamic component ~ part of blueprint.
		HOUDINI_LOG_MESSAGE(TEXT("Dynamic::OnRegister"));
	}
}


void
UHoudiniAssetComponent::OnUnregister()
{
	Super::OnUnregister();
	HOUDINI_LOG_MESSAGE(TEXT("Unregistering component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


void
UHoudiniAssetComponent::OnComponentCreated()
{
	// This event will only be fired for native Actor and native Component.
	Super::OnComponentCreated();
	HOUDINI_LOG_MESSAGE(TEXT("Creating component, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


FName
UHoudiniAssetComponent::GetComponentInstanceDataType() const
{
	// Called before we throw away components during RerunConstructionScripts, to cache any data we wish to persist across that operation.

	return Super::GetComponentInstanceDataType();
	HOUDINI_LOG_MESSAGE(TEXT("Requesting data for caching, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


void
UHoudiniAssetComponent::ApplyComponentInstanceData(TSharedPtr<class FComponentInstanceDataBase> ComponentInstanceData)
{
	// Called after we create new components during RerunConstructionScripts, to optionally apply any data backed up during GetComponentInstanceData.

	Super::ApplyComponentInstanceData(ComponentInstanceData);
	HOUDINI_LOG_MESSAGE(TEXT("Restoring data from caching, Component = 0x%0.8p, HoudiniAsset = 0x%0.8p"), this, HoudiniAsset);
}


void
UHoudiniAssetComponent::OnPreSaveWorld(uint32 SaveFlags, class UWorld* World)
{
	if(!PatchedClass)
	{
		// If class information has not been patched, do nothing.
		return;
	}

	// We need to add our patched class to root in order to avoid its clean up by GC.
	PatchedClass->AddToRoot();

	// We need to restore original class information.
	ReplaceClassObject(UHoudiniAssetComponent::StaticClass());
}


void
UHoudiniAssetComponent::OnPostSaveWorld(uint32 SaveFlags, class UWorld* World, bool bSuccess)
{
	if(!PatchedClass)
	{
		return;
	}

	// We need to restore patched class information.
	ReplaceClassObject(PatchedClass);

	// We can put our patched class back, and remove it from root as it no longer under threat of being cleaned up by GC.
	PatchedClass->RemoveFromRoot();
}


void
UHoudiniAssetComponent::PreSave()
{
	Super::PreSave();
	bPreSaveTriggered = true;
}


void
UHoudiniAssetComponent::PostLoad()
{
	Super::PostLoad();

	if(!PatchedClass && (UHoudiniAssetComponent::StaticClass() == GetClass()))
	{
		// Replace class information.
		ReplaceClassInformation(HoudiniAssetActorOwner->GetActorLabel());

		// These are used to track and insert properties into new class object.
		UProperty* PropertyFirst = nullptr;
		UProperty* PropertyLast = PropertyFirst;

		UField* ChildFirst = nullptr;
		UField* ChildLast = ChildFirst;

		// We can start reconstructing properties.
		for(TArray<FHoudiniEngineSerializedProperty>::TIterator Iter = SerializedProperties.CreateIterator(); Iter; ++Iter)
		{
			FHoudiniEngineSerializedProperty& SerializedProperty = *Iter;

			// Create unique property name to avoid collisions.
			FString UniquePropertyName = ObjectTools::SanitizeObjectName(FString::Printf(TEXT("%s_%s"), *PatchedClass->GetName(), *SerializedProperty.Name));

			// Create property.
			UProperty* Property = CreateProperty(PatchedClass, UniquePropertyName, SerializedProperty.Flags, SerializedProperty.Type);

			if(!Property) continue;

			// Set rest of property flags.
			Property->ArrayDim = SerializedProperty.ArrayDim;
			Property->ElementSize = SerializedProperty.ElementSize;

			// Set any meta information.
			if(SerializedProperty.Meta.Num())
			{
				for(TMap<FName, FString>::TConstIterator ParamIter(SerializedProperty.Meta); ParamIter; ++ParamIter)
				{
					Property->SetMetaData(ParamIter.Key(), *(ParamIter.Value()));
				}
			}

			// Replace offset value for this property.
			ReplacePropertyOffset(Property, SerializedProperty.Offset);

			// Insert this newly created property in link list of properties.
			if(!PropertyFirst)
			{
				PropertyFirst = Property;
				PropertyLast = Property;
			}
			else
			{
				PropertyLast->PropertyLinkNext = Property;
				PropertyLast = Property;
			}

			// Insert this newly created property into link list of children.
			if(!ChildFirst)
			{
				ChildFirst = Property;
				ChildLast = Property;
			}
			else
			{
				ChildLast->Next = Property;
				ChildLast = Property;
			}

			// We also need to add this property to a set of changed properties.
			if(SerializedProperty.bChanged)
			{
				ChangedProperties.Add(Property);
			}
		}

		// We can remove all serialized stored properties.
		SerializedProperties.Reset();

		// And add new created properties to our newly created class.
		UClass* ClassOfUHoudiniAssetComponent = UHoudiniAssetComponent::StaticClass();

		if(PropertyFirst)
		{
			PatchedClass->PropertyLink = PropertyFirst;
			PropertyLast->PropertyLinkNext = ClassOfUHoudiniAssetComponent->PropertyLink;
		}

		if(ChildFirst)
		{
			PatchedClass->Children = ChildFirst;
			ChildLast->Next = ClassOfUHoudiniAssetComponent->Children;
		}

		// Update properties panel.
		//UpdateEditorProperties();

		// Collect all textures (for debugging purposes).
		//CollectTextures();

		// Create all rendering resources.
		CreateRenderingResources();

		// Need to update rendering information.
		UpdateRenderingInformation();
	}
}


void
UHoudiniAssetComponent::Serialize(FArchive& Ar)
{
	Super::Serialize(Ar);

	if(Ar.IsTransacting())
	{
		// We have no support for transactions (undo system) right now.
		return;
	}

	if(!Ar.IsSaving() && !Ar.IsLoading())
	{
		return;
	}

	/*
	if(Ar.IsSaving())
	{
		if(!bPreSaveTriggered)
		{
			// Save is triggered multiple times, we want to avoid unnecessary serialization. Only save when PreSave has been called.
			Super::Serialize(Ar);
			return;
		}

		bPreSaveTriggered = false;
	}
	*/

	// State of this component.
	EHoudiniAssetComponentState::Type ComponentState = EHoudiniAssetComponentState::Invalid;

	if(Ar.IsSaving())
	{
		if(-1 != AssetId)
		{
			// Asset has been previously instantiated.

			if(HapiGUID.IsValid())
			{
				// Asset is being re-cooked asynchronously.
				ComponentState = EHoudiniAssetComponentState::BeingCooked;
			}
			else
			{
				// We have no pending asynchronous cook requests.
				ComponentState = EHoudiniAssetComponentState::Instantiated;
			}
		}
		else
		{
			if(HoudiniAsset)
			{
				// Asset has not been instantiated and therefore must have asynchronous instantiation request in progress.
				ComponentState = EHoudiniAssetComponentState::None;
			}
			else
			{
				// Component is in invalid state (for example is a default class object).
				ComponentState = EHoudiniAssetComponentState::Invalid;
			}
		}
	}

	// Serialize component state.
	Ar << ComponentState;

	// If component is in invalid state, we can skip the rest of serialization.
	if(EHoudiniAssetComponentState::Invalid == ComponentState)
	{
		return;
	}

	// Serialize asset information (package and name).
	FString HoudiniAssetPackage;
	FString HoudiniAssetName;

	if(Ar.IsSaving())
	{
		// Retrieve package and its name.
		UPackage* Package = Cast<UPackage>(HoudiniAsset->GetOuter());
		check(Package);
		Package->GetName(HoudiniAssetPackage);

		// Retrieve name of asset.
		HoudiniAssetName = HoudiniAsset->GetName();
	}

	// Serialize package name and object name - we will need those to reconstruct / locate the asset.
	Ar << HoudiniAssetPackage;
	Ar << HoudiniAssetName;

	// Serialize scratch space size.
	int64 ScratchSpaceSize = HOUDINIENGINE_ASSET_SCRATCHSPACE_SIZE;
	Ar << ScratchSpaceSize;

	if(Ar.IsLoading())
	{
		// Make sure scratch space size is suitable. We need to check this because size is defined by compile time constant.
		check(ScratchSpaceSize <= HOUDINIENGINE_ASSET_SCRATCHSPACE_SIZE);
	}

	// Serialize scratch space itself.
	Ar.Serialize(&ScratchSpaceBuffer[0], ScratchSpaceSize);

	// Number of properties.
	int PropertyCount = CreatedProperties.Num();
	Ar << PropertyCount;

	// These are used to track and insert properties into new class object.
	UProperty* PropertyFirst = nullptr;
	UProperty* PropertyLast = PropertyFirst;

	UField* ChildFirst = nullptr;
	UField* ChildLast = ChildFirst;

	for(int PropertyIdx = 0; PropertyIdx < PropertyCount; ++PropertyIdx)
	{
		// Property corresponding to this index.
		UProperty* Property = nullptr;

		// Property fields we need to serialize.
		EHoudiniEngineProperty::Type PropertyType = EHoudiniEngineProperty::None;
		int32 PropertyArrayDim = 1;
		int32 PropertyElementSize = 4;
		uint64 PropertyFlags = 0u;
		int32 PropertyOffset = 0;
		FString PropertyName;
		bool PropertyChanged = false;
		TMap<FName, FString> PropertyMeta;
		TMap<FName, FString>* LookupPropertyMeta = nullptr;

		if(Ar.IsSaving())
		{
			// Get property at this index.
			Property = CreatedProperties[PropertyIdx];
			PropertyType = GetPropertyType(Property);
			PropertyArrayDim = Property->ArrayDim;
			PropertyElementSize = Property->ElementSize;
			PropertyFlags = Property->PropertyFlags;
			PropertyOffset = Property->GetOffset_ForDebug();

			// Retrieve name of this property.
			check(Property->HasMetaData(TEXT("HoudiniParmName")));
			PropertyName = Property->GetMetaData(TEXT("HoudiniParmName"));

			// Retrieve changed status of this property, this is optimization to avoid uploading back all properties
			// to Houdini upon loading.
			if(Property->HasMetaData(TEXT("HoudiniPropertyChanged")))
			{
				PropertyChanged = true;
			}

			if(EHoudiniEngineProperty::None == PropertyType)
			{
				// We have encountered an unsupported property type.
				check(false);
			}
		}

		// Serialize fields.
		Ar << PropertyType;
		Ar << PropertyName;
		Ar << PropertyArrayDim;
		Ar << PropertyElementSize;
		Ar << PropertyFlags;
		Ar << PropertyOffset;
		Ar << PropertyChanged;

		// Serialize any meta information for this property.
		bool PropertyMetaFound = false;

		if(Ar.IsSaving())
		{
			LookupPropertyMeta = UMetaData::GetMapForObject(Property);
			PropertyMetaFound = (LookupPropertyMeta != nullptr);
		}

		Ar << PropertyMetaFound;

		if(Ar.IsSaving() && LookupPropertyMeta)
		{
			// Save meta information associated with this property.
			Ar << *LookupPropertyMeta;
		}
		else if(Ar.IsLoading())
		{
			// Load meta information for this property.
			Ar << PropertyMeta;

			// Make sure changed meta flag does not get serialized back.
			PropertyMeta.Remove(TEXT("HoudiniPropertyChanged"));
		}

		if(EHoudiniEngineProperty::String == PropertyType)
		{
			// If it is a string property, we need to reconstruct string in case of loading.
			FString* UnrealString = (FString*) ((const char*) this + PropertyOffset);

			if(Ar.IsSaving())
			{
				Ar << *UnrealString;
			}
			else if(Ar.IsLoading())
			{
				FString StoredString;
				Ar << StoredString;
				new(UnrealString) FString(StoredString);
			}
		}

		// At this point if we are loading, we can construct intermediate object.
		if(Ar.IsLoading())
		{
			FHoudiniEngineSerializedProperty SerializedProperty(PropertyType, PropertyName, PropertyFlags,
																PropertyArrayDim, PropertyElementSize, PropertyOffset, 
																PropertyChanged);
			if(PropertyMeta.Num())
			{
				SerializedProperty.Meta = PropertyMeta;
			}

			// Store property in a list.
			SerializedProperties.Add(SerializedProperty);
		}
	}

	/*
	// If this is an enum property, we need to serialize its associated enum.
	if(EHoudiniEngineProperty::Enumeration == PropertyType)
	{
		UByteProperty* EnumProperty = Cast<UByteProperty>(Property);
		UEnum* Enum = EnumProperty->Enum;

		// Retrieve name of this enum and store it.
		FString EnumName = Enum->GetName();
		Ar << EnumName;

		// Get number of enum entries (skip one because of auto generated MAX entry).
		int32 EnumEntryCount = Enum->NumEnums() - 1;
		Ar << EnumEntryCount;

		for(int EnumEntryIdx = 0; EnumEntryIdx < EnumEntryCount; ++EnumEntryIdx)
		{
			// Get name for this entry and store it.
			FString EnumEntryName = Enum->GetEnumName(EnumEntryIdx);
			Ar << EnumEntryName;

			// We also need to serialize meta data for this entry.
			{
				FString EnumEntryMeta = Enum->GetMetaData(TEXT("DisplayName"), EnumEntryIdx);
				Ar << EnumEntryMeta;
			}
			{
				FString EnumEntryMeta = Enum->GetMetaData(TEXT("HoudiniName"), EnumEntryIdx);
				Ar << EnumEntryMeta;
			}
		}
	}
	*/

	// Serialize geos.
	int32 NumGeos = HoudiniAssetObjectGeos.Num();
	Ar << NumGeos;

	for(int32 GeoIdx = 0; GeoIdx < NumGeos; ++GeoIdx)
	{
		FHoudiniAssetObjectGeo* HoudiniAssetObjectGeo = nullptr;

		if(Ar.IsSaving())
		{
			HoudiniAssetObjectGeo = HoudiniAssetObjectGeos[GeoIdx];
		}
		else if(Ar.IsLoading())
		{
			HoudiniAssetObjectGeo = new FHoudiniAssetObjectGeo();
		}

		check(HoudiniAssetObjectGeo);
		HoudiniAssetObjectGeo->Serialize(Ar);

		if(Ar.IsLoading())
		{
			HoudiniAssetObjectGeos.Add(HoudiniAssetObjectGeo);
		}
	}

	if(Ar.IsLoading())
	{
		// This component has been loaded.
		bLoadedComponent = true;

		// We need to locate corresponding package and load it if it is not loaded.
		UPackage* Package = FindPackage(NULL, *HoudiniAssetPackage);
		if(!Package)
		{
			// Package was not loaded previously, we will try to load it.
			Package = PackageTools::LoadPackage(HoudiniAssetPackage);
		}

		if(!Package)
		{
			// Package does not exist - this is a problem, we cannot continue.
			check(Package);
			return;
		}

		// At this point we can locate the asset, since package exists.
		UHoudiniAsset* HoudiniAssetLookup = Cast<UHoudiniAsset>(StaticFindObject(UHoudiniAsset::StaticClass(), Package, *HoudiniAssetName, true));
		if(HoudiniAssetLookup)
		{
			// Set asset for this component. This will trigger asynchronous instantiation.
			SetHoudiniAsset(HoudiniAssetLookup);
		}
		else
		{
			// Asset by this name does not exist in package - this is a problem, we cannot continue.
			check(HoudiniAssetLookup);
		}
	}
}
