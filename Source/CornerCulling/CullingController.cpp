// Fill out your copyright notice in the Description page of Project Settings.

#include "CullingController.h"
#include "CornerCullingCharacter.h"
#include "OccludingCuboid.h"
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
		Characters.Add(Player);
		IsAlive.Emplace(true);
		Teams.Emplace(Player->Team);
    }
	// Acquire the prisms of occluding objects.
    for (AOccludingCuboid* Occluder : TActorRange<AOccludingCuboid>(GetWorld()))
    {
		Cuboids.Add(Cuboid(Occluder->Vectors));
    }
}

void ACullingController::UpdateCharacterBounds()
{
	Bounds.Reset();
	for (int i = 0; i < Characters.Num(); i++)
    {
		if (IsAlive[i])
        {
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
	for (int i = 0; i < Characters.Num(); i++)
    {
		if (IsAlive[i])
        {
			for (int j = 0; j < Characters.Num(); j++)
            {
				if (VisibilityTimers[i][j] > 0)
                {
					VisibilityTimers[i][j]--;
				}
				if (IsAlive[j] && (Teams[i] != Teams[j]) && (VisibilityTimers[i][j] == 0))
                {
					BundleQueue.Emplace(Bundle(i, j));
				}
			}
		}
	}
}

void ACullingController::BenchmarkCull()
{
	auto Start = std::chrono::high_resolution_clock::now();
	Cull();
	auto Stop = std::chrono::high_resolution_clock::now();
	SendLocations();
	int Delta = std::chrono::duration_cast<std::chrono::microseconds>(Stop - Start).count();
	TotalTime += Delta;
	RollingTotalTime += Delta;
	RollingMaxTime = std::max(RollingMaxTime, Delta);
	if ((TotalTicks % RollingWindowLength) == 0)
    {
		RollingAverageTime = RollingTotalTime / RollingWindowLength;
		if (GEngine)
        {
			// One cull happens every CullingPeriod frames.
            // TODO:
            //   When running multiple servers per CPU,
            //   stagger culling periods so that lag spikes do not build up.
			FString Msg = "Average time to cull (microseconds): " + FString::FromInt(int(TotalTime / TotalTicks));
			GEngine->AddOnScreenDebugMessage(1, 1.0f, FColor::Yellow, Msg, true, FVector2D(1.5f, 1.5f));
			Msg = "Rolling average time to cull (microseconds): " + FString::FromInt(int(RollingAverageTime));
			GEngine->AddOnScreenDebugMessage(2, 1.0f, FColor::Yellow, Msg, true, FVector2D(1.5f, 1.5f));
			Msg = "Rolling max time to cull (microseconds): " + FString::FromInt(RollingMaxTime);
			GEngine->AddOnScreenDebugMessage(3, 1.0f, FColor::Yellow, Msg, true, FVector2D(1.5f, 1.5f));
		}
		if (RollingMaxTime > TimerLoadThreshold)
        {
			TimerIncrement = MaxTimerIncrement;
		}
		else {
			TimerIncrement = MinTimerIncrement;
		}
		RollingTotalTime = 0;
		RollingMaxTime = 0;
	}
}

void ACullingController::Cull()
{
	if ((TotalTicks % CullingPeriod) == 0)
    {
		UpdateCharacterBounds();
		PopulateBundles();
		CullWithCache();
		CullWithSpheres();
		CullWithCuboids();
	}
	// SendLocations();  // Moved to BenchmarkCull to not affect runtime statistics.
}

void ACullingController::CullWithCache()
{
	BundleQueue2.Reset();
	for (Bundle B : BundleQueue)
    {
		bool Blocked = false;
		for (int k = 0; k < CUBOID_CACHE_SIZE; k++)
        {
			if (IsBlocking(B, Cuboids[CuboidCaches[B.PlayerI][B.EnemyI][k]]))
            {
				Blocked = true;
				CacheTimers[B.PlayerI][B.EnemyI][k] = TotalTicks;
				break;
			}
		}
		if (!Blocked)
        {
			BundleQueue2.Emplace(B);
		}
	}
}

void ACullingController::CullWithSpheres()
{
	BundleQueue.Reset();
	for (Bundle B : BundleQueue2)
    {
		bool Blocked = false;
        for (Sphere S: Spheres)
        {
			if (IsBlocking(B, S))
            {
				Blocked = true;
				break;
			}
		}
		if (!Blocked)
        {
			BundleQueue.Emplace(B);
		}
	}
}

void ACullingController::CullWithCuboids()
{
	for (Bundle B : BundleQueue)
    {
		bool Blocked = false;
		for (int CuboidI : GetPossibleOccludingCuboids(B))
{
			if (IsBlocking(B, Cuboids[CuboidI]))
            {
				Blocked = true;
				int MinI = ArgMin(CacheTimers[B.PlayerI][B.EnemyI], CUBOID_CACHE_SIZE);
				CuboidCaches[B.PlayerI][B.EnemyI][MinI] = CuboidI;
				CacheTimers[B.PlayerI][B.EnemyI][MinI] = TotalTicks;
				break;
			}
		}
		if (!Blocked)
        {
			// Random offset spreads out culling when all characters become visible
			// to each other at the same time, such as when a smoke fades.
			VisibilityTimers[B.PlayerI][B.EnemyI] = (
				TimerIncrement + (FMath::Rand() % 2)
			);
		}
	}
}


TArray<FVector> ACullingController::GetPossiblePeeks(
    FVector PlayerCameraLocation,
    FVector EnemyLocation,
    float MaxDeltaHorizontal,
    float MaxDeltaVertical)
{
    TArray<FVector> Corners;
    FVector PlayerToEnemy = (EnemyLocation - PlayerCameraLocation).GetSafeNormal(1e-6);
	// Horizontal vector is parallel to the XY plane and is perpendicular to PlayerToEnemy.
	FVector Horizontal = MaxDeltaHorizontal * FVector(-PlayerToEnemy.Y, PlayerToEnemy.X, 0);
	FVector Vertical = FVector(0, 0, MaxDeltaVertical);
	Corners.Emplace(PlayerCameraLocation + Horizontal + Vertical);
	Corners.Emplace(PlayerCameraLocation - Horizontal + Vertical);
	Corners.Emplace(PlayerCameraLocation - Horizontal - Vertical);
	Corners.Emplace(PlayerCameraLocation + Horizontal - Vertical);
    return Corners;
}

// Get all faces that sit between a player and an enemy and have a normal pointing outward
// toward the player, thus skipping redundant back faces.
TArray<Face> ACullingController::GetFacesBetween(
	const FVector& PlayerCameraLocation,
	const FVector& EnemyCenter,
	const Cuboid& OccludingCuboid)
{
    TArray<Face> FacesBetween;
	for (int i = 0; i < CUBOID_F; i++)
    {
		Face F = OccludingCuboid.Faces[i];
		FVector FaceV = OccludingCuboid.GetVertex(i, 0);
		FVector PlayerToFace = FaceV - PlayerCameraLocation;
		FVector EnemyToFace = FaceV - EnemyCenter;
		if (   (FVector::DotProduct(PlayerToFace, F.Normal) < 0)
			&& (FVector::DotProduct(EnemyToFace, F.Normal) > 0))
        {
			FacesBetween.Emplace(F);
		}
	}
    return FacesBetween;
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
	TArray<FPlane>& ShadowFrustum)
{
	// Reset the edge set.
	memset(EdgeSet, false, 64);
	// Add all perimeter edges of all faces to the EdgeSet.
	for (const Face& F : FacesBetween)
    {
		EdgeSet[F.Perimeter[0]][F.Perimeter[1]] = true;
		EdgeSet[F.Perimeter[1]][F.Perimeter[2]] = true;
		EdgeSet[F.Perimeter[2]][F.Perimeter[3]] = true;
		EdgeSet[F.Perimeter[3]][F.Perimeter[0]] = true;
	}
	// For all unpaired, occluding edges, create a corresponding
	// shadow frustum plane, and add it to the ShadowFrustum.
	for (const Face& F : FacesBetween)
    {
		// Indices of vertices that define the perimeter of the face.
		int V0 = F.Perimeter[0];
		int V1 = F.Perimeter[1];
		int V2 = F.Perimeter[2];
		int V3 = F.Perimeter[3];
		// If edge (j, i) is not present, create a plane with (i, j).
		if (!EdgeSet[V1][V0])
        {
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V0],
				OccludingCuboid.Vertices[V1]
			));
		}
		if (!EdgeSet[V2][V1])
        {
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V1],
				OccludingCuboid.Vertices[V2]
			));
		}
		if (!EdgeSet[V3][V2])
        {
			FVector u = OccludingCuboid.Vertices[V2] + FVector(0, 0, 10);
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V2],
				OccludingCuboid.Vertices[V3]
			));
		}
		if (!EdgeSet[V0][V3])
        {
			ShadowFrustum.Emplace(FPlane(
				PlayerCameraLocation,
				OccludingCuboid.Vertices[V3],
				OccludingCuboid.Vertices[V0]
			));
		}
	}
}


bool ACullingController::IsBlocking(const Bundle& B, Sphere& OccludingSphere)
{
    return false;
}

// Check if the Cuboid blocks visibility between a bundle's player and enemy.
// For each of the most aggressive peeks a player camera could perform on
// the enemy within the latency period:
//   Try to use the enemy's bounding spheres to quickly check visibility.
//   If visibility is still ambiguous, check all points of the bounding box.
// Return whether all potential peeks are blocked.
bool ACullingController::IsBlocking(const Bundle& B, Cuboid& OccludingCuboid)
{
    FVector& PlayerCameraLocation = Bounds[B.PlayerI].CameraLocation;
    CharacterBounds& EnemyBounds = Bounds[B.EnemyI];
    FVector& EnemyCenter = EnemyBounds.Center;
    float EnemyRadius = EnemyBounds.BoundingSphereRadius;
    // TODO: Make displacement a function of latency and game state.
    TArray<FVector> Peeks = GetPossiblePeeks(
        PlayerCameraLocation,
        EnemyCenter,
        20.0f,  // Maximum horizontal displacement
        5.0f  // Maximum vertical displacement
    );
    // Shadow frustum for each possible peek.
    TArray<FPlane> ShadowFrustums[NUM_PEEKS] = { TArray<FPlane>() };
    for (int i = 0; i < NUM_PEEKS; i++)
    {
        // Get the faces of the cuboid that are visible to the player at Peeks[i]
        // and in between the player at Peeks[i] and the enemy.
        TArray<Face> FacesBetween = GetFacesBetween(
            Peeks[i], EnemyBounds.Center, OccludingCuboid
        );
        if (FacesBetween.Num() > 0)
        {
            GetShadowFrustum(Peeks[i], OccludingCuboid, FacesBetween, ShadowFrustums[i]);
            // Planes of the shadow frustum that clip the enemy bounding sphere
            TArray<FPlane> ClippingPlanes;
            // Try to determine visibility with quick bounding sphere checks.
            for (FPlane& P : ShadowFrustums[i])
            {
                // Signed distance from enemy to plane.
                // The direction of the plane's normal vector is negative.
                float EnemyDistanceToPlane = -P.PlaneDot(EnemyCenter);
                // The bounding sphere is in the inner half space of this plane.
                if (EnemyDistanceToPlane > EnemyRadius)
                {
                    continue;
                    // Use the bounding box to determine if the enemy is blocked.
                }
                else {
                    ClippingPlanes.Emplace(P);
                }
            }
            // Check if the vertices of the enemy bounding box are in all half spaces
            // defined by clipping planes.
            // Because each bounding box bottom vertex is directly below a top vertex,
            // we do not need to check bottom vertices when peeking from above.
            // Likewise for top vertices.
            if (   (i < 2)
                && !InHalfSpaces(EnemyBounds.TopVertices, ClippingPlanes))
            {
            	return false;
            }
            if (   (i >= 2)
                && !InHalfSpaces(EnemyBounds.BottomVertices, ClippingPlanes))
            {
            	return false;
            }
            // There are no faces between the player and enemy.
            // Thus the cuboid cannot block LOS.
        }
        // No faces between the player and enemy. The cuboid cannot block LOS.
        else {
            return false;
        }
    }
	return true;
}

// For each plane, define a half-space by the set of all points
// with a positive dot product with its normal vector.
// Check that every point is within all half-spaces.
bool ACullingController::InHalfSpaces(
	const TArray<FVector>& Points,
	const TArray<FPlane>& Planes)
{
	for (const FVector& Point : Points)
    {
		for (const FPlane& Plane : Planes)
        {
			// The point is on the outer side of the plane.
			if (Plane.PlaneDot(Point) > 0)
            {
				return false;
			}
		}
	}
	return true;
}

// Currently just returns all cuboids.
// TODO: Implement search through Bounding Volume Hierarchy.
TArray<int> ACullingController::GetPossibleOccludingCuboids(Bundle B)
{
	TArray<int> PossibleOccluders;
	for (int i = 0; i < Cuboids.Num(); i++)
    {
		PossibleOccluders.Emplace(i);
	}
	return PossibleOccluders;
}

void ACullingController::SendLocations()
{
	for (int i = 0; i < Characters.Num(); i++)
    {
		if (IsAlive[i])
        {
			for (int j = 0; j < Characters.Num(); j++)
            {
				if (IsAlive[j] && (VisibilityTimers[i][j] > 0))
                {
					SendLocation(i, j);
				}
			}
		}
	}
}

// Draw a line from character i to j, simulating the sending of a location.
// TODO: Integrate server location-sending API when deploying to a game.
void ACullingController::SendLocation(int i, int j)
{
	// Only draw sight lines of team 0.
	if (Teams[i] == 0)
    {
		ConnectVectors(
			GetWorld(),
			Bounds[i].Center + FVector(0, 0, 10),
			Bounds[j].Center,
			false,
			0.02,
			1,
			FColor::Green);
	}
}

void ACullingController::Tick(float DeltaTime)
{
	TotalTicks++;
	BenchmarkCull();
}


