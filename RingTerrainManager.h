#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Curves/CurveFloat.h"
#include "FoliageType_InstancedStaticMesh.h"
#include <KismetProceduralMeshLibrary.h>
#include "ProceduralMeshComponent.h"
#include "Engine/InstancedStaticMesh.h"

#include "RingTerrainManager.generated.h"


class UProceduralMeshComponent;
class USceneComponent;

USTRUCT(BlueprintType)
struct FTerrainParams
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
    float BaseScale = 0.0000667f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
    float HeightScale = 20000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
    int32 Octaves = 5;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
    float Lacunarity = 2.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
    float Gain = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Noise")
    float Ridge = 0.0001f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain")
    float ChunkWorldSize = 1000.0f;
};

USTRUCT(BlueprintType)
struct FFoliageSpawnData
{
    GENERATED_BODY()
    FVector Location;
    TSubclassOf<AActor> FoliageClass;
};

USTRUCT(BlueprintType)
struct FGrassSpawnData
{
    GENERATED_BODY()
    FVector Location;
    TSubclassOf<AActor> GrassClass;
};

USTRUCT(BlueprintType)
struct FBuildingSpawnData
{
    GENERATED_BODY()
    FVector Location;
    TSubclassOf<AActor> BuildingClass;
};



USTRUCT(BlueprintType)
struct FBuildingSpawn
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TSubclassOf<AActor> BuildingClass;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector Location;

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    FVector2D HalfExtents = FVector2D(500, 500);

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float FlattenHeightOffset = 0.0f;
};



struct FRingHeightField
{
    int32 SizeX = 0;
    int32 SizeY = 0;
    TArray<float> Heights; // row-major: Heights[y * SizeX + x]

    void Resize(int32 SX, int32 SY)
    {
        SizeX = SX; SizeY = SY;
        Heights.SetNumZeroed(SX * SY);
    }

    FORCEINLINE float& At(int32 X, int32 Y) { return Heights[Y * SizeX + X]; }
    FORCEINLINE const float& At(int32 X, int32 Y) const { return Heights[Y * SizeX + X]; }
};

struct FRoadDFCell
{
    float Dist = FLT_MAX;   // nearest spline distance
    float Height = 0.0f;    // interpolated road height at the nearest point
};

struct FRingMeshBuildData
{
    FIntPoint ChunkCoord;
    int32 LOD;

    TArray<FVector> Vertices;
    TArray<FVector2D> UVs;
    TArray<FLinearColor> VertexColors;
    TArray<FVector> Normals;
    TArray<int32> TerrainTriangles;
    TArray<int32> RoadTriangles;
    TArray<FProcMeshTangent> Tangents;

    // Distance field baked per chunk (small grid) to accelerate road queries in tight vertex loops
    TArray<FRoadDFCell> RoadDistanceField;

    TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HeightField;
};

/** Road spline for procedural carving */
USTRUCT(BlueprintType)
struct FRoadSpline
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TArray<FVector> ControlPoints; // world-space points

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Width = 800.f;
};


/* ---------- RIVERS ---------- */
USTRUCT(BlueprintType)
struct FRiverSpline
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    TArray<FVector> ControlPoints; // world-space points (Z ignored)

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float Width = 1500.f; // river width

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float WaterLevel = -15000.f; // fixed elevation for river bed

    UPROPERTY(EditAnywhere, BlueprintReadWrite)
    float BankBlendStrength = 0.4f; // how smooth the banks blend to terrain (0..1)

    // Infinite growth support
    UPROPERTY()
    bool bCanExtend = true;
};
UCLASS(Blueprintable)
class WYRD55_API ARingTerrainManager : public AActor
{
    GENERATED_BODY()

public:
    ARingTerrainManager();

    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;

    /* Editor-exposed settings */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Global")
    int32 ReferenceResolution = 4338;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Global")
    float ReferenceSize = 200000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Global")
    float MountainScale = 0.00005f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Global")
    float MountainStrength = 0.4f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Global")
    float MountainHeight = 666000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Chunk")
    int32 ChunkResolution = 64;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
    int32 ViewRadiusInChunks = 20;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|LOD")
    TArray<int32> LODResolutions = { 129, 65, 33, 17 };

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|LOD")
    TArray<float> LODDistanceBands = { 1024.f, 3072.f, 8192.f, 20000.f };

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    FTerrainParams Params;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    UCurveFloat* NoiseCurve = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    int32 NoiseTileResolution = 1024;

    // Precomputed noise tile for fast FBM sampling (size = NoiseTileResolution^2)
    TArray<float> NoiseTile;

    // Splat / biome curves (procedural splat map uses these per-channel)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    TArray<UCurveFloat*> SplatCurves;

    // Use a procedurally generated splat map instead of a texture
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    bool bUseProceduralSplatMap = true;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    float SplatMapScale = 0.00001f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    float SplatMapFrequency = 0.1f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Visual")
    UMaterialInterface* ChunkMaterial = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Visual")
    UMaterialInterface* RoadMaterial = nullptr;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Performance")
    int32 MaxPoolSize = 64;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Noise")
    int32 NoiseCurveTableSize = 1024;

    UFUNCTION(BlueprintCallable, Category = "Terrain")
    void RegenerateAll();

    /* Foliage + Grass */
    UPROPERTY(EditAnywhere, Category = "Foliage")
    TArray<TSubclassOf<AActor>> FoliageActors;
    TQueue<FFoliageSpawnData, EQueueMode::Mpsc> FoliageSpawnQueue;
    void ProcessFoliageSpawns();

    UPROPERTY(EditAnywhere, Category = "Foliage")
    UInstancedStaticMeshComponent* Tree_Inst;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    double TreeDensity = 0.00666;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float TreeSlope = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float TreeAltitudeMin = -19500.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float TreeAltitudeMax = -5000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    TArray<TSubclassOf<AActor>> GrassActors;
    TQueue<FGrassSpawnData, EQueueMode::Mpsc> GrassSpawnQueue;
    void ProcessGrassSpawn();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    double GrassDensity = 0.01;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float GrassSlope = 0.4f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float GrassAltitudeMin = -19800.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float GrassAltitudeMax = -19000.0f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Buildings")
    TArray<FBuildingSpawn> Buildings;

    TMap<FVector, AActor*> SpawnedBuildingActors;
    bool IsInsideBuildingFootprint(const FVector& WorldPos, const FBuildingSpawn& Building, float Margin = 0) const;
    void SpawnBuildingsIfNeeded();

    /* Roads */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    TArray<FRoadSpline> RoadSplines;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Rivers")
    TArray<FRiverSpline> RiverSplines;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Rivers")
    float GlobalRiverWaterLevel = -15000.f;


    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    int32 NumRandomRoads = 5;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    int32 RoadPointsPerSpline = 20;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    float RoadSegmentLength = 5000.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    int32 RandomSeed = 1337;

    /** Global multiplier to scale all road widths at runtime/editor without editing each spline */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    float RoadWidthMultiplier = 1.0f;

    /** 0 = sharp/hard road edge, 1 = smooth falloff according to SmoothStep.
      * Lower values produce a sharper transition (using a power curve internally). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads", meta = (ClampMin = "0.0", ClampMax = "1.0"))
    float RoadBlendStrength = 0.5f;

    /** How close trees may appear to the road edge (in world units). Trees will never spawn inside the effective road width. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Foliage")
    float TreeRoadMargin = 200.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Adaptive")
    float AdaptiveLODRadius = 5000.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "LOD|Adaptive")
    float LODFalloffExponent = 1.0f; // 1 = linear, >1 = faster dropoff, <1 = slower


    // Road distance-field resolution for per-chunk acceleration (small grid, bilinearly sampled)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Roads")
    int32 RoadDistanceFieldResolution = 32;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Streaming")
    float StreamingUpdateInterval = 0.2f; // 5 Hz

    UFUNCTION(BlueprintCallable, Category = "Terrain|Rivers")
    void GenerateRandomRivers();

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Buildings")
    float BuildingNoSpawnRadius = 2000.f;   // distance around buildings where trees/grass cannot spawn

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Buildings")
    float BuildingDensity = 0.00005f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Buildings")
    float BuildingMinSlope = 0.5f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Buildings")
    float BuildingAltitudeMin = -20000.f;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Terrain|Buildings")
    float BuildingAltitudeMax = -15000.f;


private:
    /* Derived runtime params */
    float VertexSpacing = 1.0f;
    float ChunkWorldSize = 1.0f;

    /* Runtime state */
    TMap<FIntPoint, UProceduralMeshComponent*> ActiveChunks;
    TMap<FIntPoint, int32> ChunkLODMap;
    TArray<UProceduralMeshComponent*> MeshPool;
    /** All spawn actors per-chunk (trees & grass) */
    TMap<FIntPoint, TArray<AActor*>> SpawnedFoliagePerChunk;


    /* Noise curve */
    TArray<float> NoiseCurveTable;

    /* Helpers */

    /* Splat curve tables (prebaked float LUTs for each curve) */
    TArray<TArray<float>> SplatCurveTables;

    /* Procedural splat map helpers */
    FColor GenerateProceduralSplat(double WX, double WY, double NormalizedHeight) const;
    float ApplySplatCurves(double Normalized, const TArray<TArray<float>>& SplatTables) const;

    /* Helpers */
    /* River helpers */
    FVector GetRiverDirection(const FVector& Pos);
    float SampleTerrainHeight(const FVector& Pos);
    FVector2D SampleTerrainGradient(const FVector& Pos);
    bool ComputeDistanceToRiver(double WX, double WY, const FRiverSpline& River, float& OutDist) const;
    void ExtendRiversIfNeeded();

    /* Helpers */
    void EnsurePoolSize(int32 Desired);
    UProceduralMeshComponent* AcquireMeshComponent();
    void ReleaseMeshComponent(UProceduralMeshComponent* Mesh);

    FIntPoint CameraChunkCoordFromLocation(const FVector& Loc) const;
    void UpdateStreaming(const FVector& PlayerLocation);

    // Async streaming system
    void UpdateStreamingWorker(const FVector& PlayerLocation);
    void ApplyStreamingChanges(const TSet<FIntPoint>& Desired, const TMap<FIntPoint, int32>& DesiredLODMap);

    void RequestChunkGeneration(const FIntPoint& ChunkCoord, int32 LOD, TMap<FIntPoint, int32> LODSnapshot);
    void BuildMeshDataWorker(FRingMeshBuildData& OutData);
    void ApplyMeshDataToComponent(UProceduralMeshComponent* Mesh, const FRingMeshBuildData& Data);
    TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> GenerateHeightfieldWorker(const FIntPoint& ChunkCoord, int32 LOD, const TMap<FIntPoint, int32>& LODSnapshot, const TArray<float>& CurveCopy, const TArray<TArray<float>>& SplatTablesCopy);
    void StitchHeightfieldEdges(TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HF, const FIntPoint& ChunkCoord, int32 LOD, const TMap<FIntPoint, int32>& LODSnapshot);

    void BuildMeshFromHeightfield(UProceduralMeshComponent* Mesh, const FIntPoint& ChunkCoord, int32 LOD, TSharedPtr<FRingHeightField, ESPMode::ThreadSafe> HF);

    float TerrainNormalizedAt(double WX, double WY) const;
    float TerrainNormalizedAt_FullFBM(double WX, double WY) const;
    // Fast noise tile sampler
    float SampleNoiseTile(double WX, double WY) const;
    // Road distance-field helpers
    void BuildRoadDistanceField(const FIntPoint& ChunkCoord, int32 DFResolution, TArray<FRoadDFCell>& OutDF);
    bool SampleRoadDF(const TArray<FRoadDFCell>& DF, int32 DFRes, double WX, double WY, const FIntPoint& ChunkCoord, float& OutDist, float& OutZ) const;

    float ApplyCurveAndScale(double Normalized) const;

    int32 PickLODForChunk(const FIntPoint& ChunkCoord, const FVector& PlayerLocation) const;

    float GetHeight(const FVector2D Location);
    float PerlinNoiseExt(const FVector2D Location, const float Scale, const float Amplitude, const FVector2D offset);
    double MountainNoise(double WX, double WY) const;

    void UpdateChunksWorker(FVector PlayerPos);

    // --- Road helpers ---
    /** Returns true if the point lies strictly within Road.Width (same as previous behavior). */
    bool GetDistanceToSpline(double WX, double WY, const FRoadSpline& Road, float& OutDist, float& OutZ) const;

    /** Unconstrained computation of distance to spline: returns nearest distance and interpolated Z for the closest point on any segment.
      * Useful when you need the true distance even if it's outside the road width. Returns false if the road has <2 control points. */
    bool ComputeDistanceToSpline(double WX, double WY, const FRoadSpline& Road, float& OutDist, float& OutZ) const;

    bool IsNearRoad(const FVector& WorldPos, const TArray<FRoadSpline>& Roads) const;

    /** Tracks which chunks already spawned their trees (prevents duplicates on LOD changes) */
    TSet<FIntPoint> ChunksWithTrees;
    TQueue<FBuildingSpawnData, EQueueMode::Mpsc> BuildingSpawnQueue;
    void ProcessBuildingSpawns();


    // --- LOD ---
    UPROPERTY(EditAnywhere, Category = "LOD")
    int32 MaxLOD = 3;

    UPROPERTY(EditAnywhere, Category = "LOD")
    int32 ChunksPerLODRing = 1;

    int32 CalculateLODFromDistance(const FIntPoint& Coord, const FIntPoint& PlayerChunk) const;
    bool bStreamingWorkerBusy = false;
    FCriticalSection StreamingLock;
    float TimeSinceLastStreamingUpdate = 0.2f;


};
