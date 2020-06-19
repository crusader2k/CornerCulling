// Fill out your copyright notice in the Description page of Project Settings.

#include "CullingController.h"
#include "CornerCullingCharacter.h"
#include "Occluder.h"
#include "EngineUtils.h" // TActorRange
#include <chrono> 

ACullingController::ACullingController()
	: Super()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;
}

void ACullingController::BeginPlay()
{
	// Call the base class  
	Super::BeginPlay();

    for (ACornerCullingCharacter* Player : TActorRange<ACornerCullingCharacter>(GetWorld()))
    {
		Characters.Emplace(Player);
		IsAlive.Emplace(true);
    }
	// Acquire the prisms of occluding objects.
    for (AOccluder* Occluder : TActorRange<AOccluder>(GetWorld()))
    {
		// Try profiling emplace.
		Cuboids.Emplace(Cuboid(Occluder->Vectors));
    }
}

void ACullingController::UpdateCharacterBounds()
{
	Bounds.Reset(Characters.Num());
	for (int i = 0; i < Characters.Num(); i++) {
		if (IsAlive[i]) {
			Bounds.Emplace(CharacterBounds(
				Characters[i]->GetFirstPersonCameraComponent()->GetComponentLocation(),
				Characters[i]->GetActorTransform()
			));
		}
	}
}

void ACullingController::PopulateBundles()
{
	BundleQueue.Reset();
	for (int i = 0; i < Characters.Num(); i++) {
		if (i != 1) 
			continue;
		if (IsAlive[i]) {
			int TeamI = Characters[i]->Team;
			for (int j = 0; j < Characters.Num(); j++) {
				if (IsAlive[j] && (TeamI != Characters[j]->Team) && (VisibilityTimers[i][j] == 0)) {
					BundleQueue.Emplace(Bundle(i, j));
				}
			}
		}
	}
}

void ACullingController::CullWithCache()
{
	BundleQueue2.Reset();
	for (Bundle B : BundleQueue) {
		bool Blocked = false;
		for (int k = 0; k < CUBOID_CACHE_SIZE; k++) {
			if (IsBlocking(B, Cuboids[CuboidCaches[B.PlayerI][B.EnemyI][k]])) {
				GEngine->AddOnScreenDebugMessage(6, 0.25f, FColor::Yellow, "EEEEE", true, FVector2D(1.5f, 1.5f));
				Blocked = true;
				CacheTimers[B.PlayerI][B.EnemyI][k] = TotalTicks;
				break;
			}
		}
		if (!Blocked) {
			// Try emplace later.
			BundleQueue2.Emplace(B);
		}
	}
}

void ACullingController::CullRemaining()
{
	for (Bundle B : BundleQueue2) {
		bool Blocked = false;
		for (int CuboidI : GetPossibleOccludingCuboids(B)) {
			if (IsBlocking(B, Cuboids[CuboidI])) {
				Blocked = true;
				int MinI = ArgMin(CacheTimers[B.PlayerI][B.EnemyI], CUBOID_CACHE_SIZE);
				CuboidCaches[B.PlayerI][B.EnemyI][MinI] = CuboidI;
				CacheTimers[B.PlayerI][B.EnemyI][MinI] = TotalTicks;
				break;
			}
		}
		if (!Blocked) {
			VisibilityTimers[B.PlayerI][B.EnemyI] += TimerIncrement;
		}
	}
}

void ACullingController::UpdateVisibility()
{
	for (int i = 0; i < Characters.Num(); i++) {
		if (IsAlive[i]) {
			for (int j = 0; j < Characters.Num(); j++) {
				if (IsAlive[j] && (VisibilityTimers[i][j] > 0)) {
					SendLocation(i, j);
					VisibilityTimers[i][j] -= 1;
				}
			}
		}
	}
}

// Get all faces that sit between a player and an enemy and have a normal pointing outward
// toward the player, thus skipping redundant back faces.
void ACullingController::GetFacesBetween(
	const FVector& PlayerCameraLocation,
	const FVector& EnemyCenter,
	const Cuboid& OccludingCuboid,
	TArray<Face>& FacesBetween
) {
	for (int i = 0; i < CUBOID_F; i++) {
		Face F = OccludingCuboid.Faces[i];
		FVector FaceV = OccludingCuboid.GetVertex(i, 0);
		FVector PlayerToFace = FaceV - PlayerCameraLocation;
		FVector EnemyToFace = FaceV - EnemyCenter;
		if (
			(FVector::DotProduct(PlayerToFace, F.Normal) < 0) &&
			(FVector::DotProduct(EnemyToFace, F.Normal) > 0)
		) {
			FacesBetween.Emplace(F);
		}
	}
}

// Gets the shadow frustum's planes, which are defined by three points:
// the player's camera location and the endpoints of an occluding, exterior edge
// of the occluding surface formed by all faces in FacesBetween.
// Edge (i, j) is an occluding, exterior edge if two conditions hold:
//   1) It is an edge of the perimeter of a face in FacesBetween
//   2) Edge (j, i) is not.
// This trick relies on fact that faces of a polyhedron have outward normals,
// and perimeter edges of faces wrap counter-clockwise by the right-hand rule.
// Thus, when two faces share an edge, that edge is included in the set of
// their edges as (i, j) from the left face and (j, i) from the right.
// Thus, interior edges of the occluding surface are identified and omitted.
void ACullingController::GetShadowFrustum(
	const FVector& PlayerCameraLocation,
	const Cuboid& OccludingCuboid,
	const TArray<Face>& FacesBetween,
	TArray<FPlane>& ShadowFrustum
) {
	// Reset the edge set.
	memset(EdgeSet, false, 64);
	// Add all perimeter edges of all faces to the EdgeSet.
	for (const Face& F : FacesBetween) {
		// Unrolled for speed. Could try optimizing further.
		EdgeSet[F.Perimeter[0]][F.Perimeter[1]] = true;
		EdgeSet[F.Perimeter[1]][F.Perimeter[2]] = true;
		EdgeSet[F.Perimeter[2]][F.Perimeter[3]] = true;
		EdgeSet[F.Perimeter[3]][F.Perimeter[0]] = true;
	}
	// For all unpaired, occluding edges, create a corresponding
	// shadow frustum plane, and add it to the ShadowFrustum.
	for (const Face& F : FacesBetween) {
		// Indices of vertices that define the perimeter of the face.
		int V0 = F.Perimeter[0];
		int V1 = F.Perimeter[1];
		int V2 = F.Perimeter[2];
		int V3 = F.Perimeter[3];
		// If edge (j, i) is not present, create a plane with (i, j).
		if (!EdgeSet[V1][V0]) {
			// TODO: Remove dead visualization code.

			//FVector u = OccludingCuboid.Vertices[V0] + FVector(0, 0, 10);
			//FVector v = OccludingCuboid.Vertices[V1] + FVector(0, 0, 10);
			//DrawDebugDirectionalArrow(GetWorld(), u, v, 100.f, FColor::Red, false, 0.2f);
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V0],
				OccludingCuboid.Vertices[V1]
			));
			//FPlane P = FPlane(
			//	PlayerCameraLocation,
			//	OccludingCuboid.Vertices[V0],
			//	OccludingCuboid.Vertices[V1]
			//);
			//FVector Start = (OccludingCuboid.Vertices[V0] + OccludingCuboid.Vertices[V1])/2;
			//DrawDebugDirectionalArrow(GetWorld(), Start, Start + 100 * P, 100.f, FColor::Red, false, 0.2f);
		}
		if (!EdgeSet[V2][V1]) {
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V1],
				OccludingCuboid.Vertices[V2]
			));
		}
		if (!EdgeSet[V3][V2]) {
			FVector u = OccludingCuboid.Vertices[V2] + FVector(0, 0, 10);
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V2],
				OccludingCuboid.Vertices[V3]
			));
		}
		if (!EdgeSet[V0][V3]) {
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V3],
				OccludingCuboid.Vertices[V0]
			));
		}
	}
}

// Check if the Cuboid blocks visibility between a bundle's player and enemy.
// For each of the most aggressive peeks a player camera could perform on
// the enemy within the latency period:
//   Try to use the enemy's bounding spheres to quickly check visibility.
//   If visibility is still ambiguous, check all points of the bounding box.
// Return whether all potential peeks are blocked.
bool ACullingController::IsBlocking(const Bundle& B, Cuboid& OccludingCuboid)
{
	// Faces of the cuboid that are between the player and enemy
	// and have a normal pointing toward the player--thus skipping back faces.
	TArray<Face> FacesBetween;
	FVector& PlayerCameraLocation = Bounds[B.PlayerI].CameraLocation;
	FVector& EnemyCenter = Bounds[B.EnemyI].Center;
	float EnemyInnerRadius = Bounds[B.EnemyI].InnerRadius;
	float EnemyOuterRadius = Bounds[B.EnemyI].OuterRadius;
	bool blocked = false;
	GetFacesBetween(PlayerCameraLocation, EnemyCenter, OccludingCuboid, FacesBetween);
	if (FacesBetween.Num() > 0) {
		blocked = true;
		TArray<FPlane> ShadowFrustum;
		GetShadowFrustum(PlayerCameraLocation, OccludingCuboid, FacesBetween, ShadowFrustum);
		//GEngine->AddOnScreenDebugMessage(9, 0.25f, FColor::Yellow, FString::FromInt(ShadowFrustumPlanes.Num()), true, FVector2D(1.5f, 1.5f));
		for (FPlane& P : ShadowFrustum) {
			// Signed distance from enemy to plane. The direction of the plane's normal vector is negative.
			float EnemyDistanceToPlane = -P.PlaneDot(EnemyCenter);
			if (EnemyDistanceToPlane > EnemyOuterRadius) {
				continue;
			} else if (EnemyDistanceToPlane < EnemyInnerRadius) {
				blocked = false;
				break;
			} else {
				GEngine->AddOnScreenDebugMessage(6, 0.25f, FColor::Yellow, "REEEE", true, FVector2D(1.5f, 1.5f));
			}
		}
	}
	return blocked;
}

// Use bounding spheres to check if a cuboid blocks visibility.
bool ACullingController::IsBlocking(
	const FVector& PlayerCameraLocation,
	const CharacterBounds& EnemyBounds
) {
	
}

// Currently just returns all cuboids.
// TODO: Implement Bounding Volume Hierarchy's.
TArray<int> ACullingController::GetPossibleOccludingCuboids(Bundle B) {
	TArray<int> Possible;
	for (int i = 0; i < Cuboids.Num(); i++) {
		Possible.Emplace(i);
	}
	return Possible;
}

// Draw a line from character i to j, simulating the sending of a location.
// TODO: Integrate server location-sending API when deploying to a game.
void ACullingController::SendLocation(int i, int j)
{
	// Only draw lines for the controlled demo character, for visibility.
	if (i != 1)
		return;
	ConnectVectors(
		GetWorld(),
		Bounds[i].Center + FVector(0, 0, 10),
		Bounds[j].Center
	);
}

void ACullingController::Tick(float DeltaTime)
{
	TotalTicks++;
	BenchmarkCull();
}

void ACullingController::Cull() {
	UpdateCharacterBounds();
	PopulateBundles();
	CullWithCache();
	CullRemaining();
	// Moved to BenchMarkCull so that debug lines do not mess up benchmark.
	// UpdateVisibility();
}

void ACullingController::BenchmarkCull() {
	auto Start = std::chrono::high_resolution_clock::now();
	Cull();
	auto Stop = std::chrono::high_resolution_clock::now();
	UpdateVisibility();
	int Delta = std::chrono::duration_cast<std::chrono::microseconds>(Stop - Start).count();
	TotalTime += Delta;
	RollingTotalTime += Delta;
	RollingMaxTime = std::max(RollingMaxTime, Delta);
	if ((TotalTicks % RollingLength) == 0) {
		RollingAverageTime = RollingTotalTime / RollingLength;
		RollingTotalTime = 0;
		RollingMaxTime = 0;
	}
	if (GEngine && (TotalTicks - 1) % RollingLength == 0) {
		// Remember, 1 cull happens per culling period. Each cull takes period times as long as the average.
		// Make sure that multiple servers are staggered so these spikes do not add up.
		FString Msg = "Average time to cull (microseconds): " + FString::FromInt(int(TotalTime / TotalTicks));
		GEngine->AddOnScreenDebugMessage(1, 0.25f, FColor::Yellow, Msg, true, FVector2D(1.5f, 1.5f));
		Msg = "Rolling average time to cull (microseconds): " + FString::FromInt(int(RollingAverageTime));
		GEngine->AddOnScreenDebugMessage(2, 0.25f, FColor::Yellow, Msg, true, FVector2D(1.5f, 1.5f));
		Msg = "Rolling max time to cull (microseconds): " + FString::FromInt(RollingMaxTime);
		GEngine->AddOnScreenDebugMessage(3, 0.25f, FColor::Yellow, Msg, true, FVector2D(1.5f, 1.5f));
	}
}

