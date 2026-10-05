#include "EndlessEnemyController.h"

#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "NavigationSystem.h"
#include "Navigation/PathFollowingComponent.h"

DEFINE_LOG_CATEGORY_STATIC(LogEndlessEnemyAI, Log, All);

AEndlessEnemyController::AEndlessEnemyController()
{
    PrimaryActorTick.bCanEverTick = false;
}

void AEndlessEnemyController::OnPossess(APawn* InPawn)
{
    Super::OnPossess(InPawn);

    State = EEndlessEnemyState::Patrol;
    bHasPatrolDestination = false;
    StuckTime = 0.0f;
    LastThinkLocation = InPawn->GetActorLocation();

    const UWorld* World = GetWorld();
    LastThinkTime = World ? World->GetTimeSeconds() : 0.0f;
    PatrolWaitUntil = 0.0f;
    NextFireTime = 0.0f;
    SetFacingMode(false);

    if (World)
    {
        GetWorldTimerManager().SetTimer(
            ThinkTimer,
            this,
            &AEndlessEnemyController::Think,
            FMath::Max(ThinkInterval, 0.05f),
            true,
            InitialDelay + FMath::FRandRange(0.0f, InitialDelayVariance));
    }
}

void AEndlessEnemyController::OnUnPossess()
{
    GetWorldTimerManager().ClearTimer(ThinkTimer);
    Super::OnUnPossess();
}

void AEndlessEnemyController::SetBrainActive(const bool bActive)
{
    bBrainActive = bActive;
    if (!bActive)
    {
        StopMovement();
        ClearFocus(EAIFocusPriority::Gameplay);
        bHasPatrolDestination = false;
    }
}

void AEndlessEnemyController::AlertToPlayer()
{
    const UWorld* World = GetWorld();
    const APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!World || !IsValid(Player) || !GetPawn())
    {
        return;
    }

    const float Now = World->GetTimeSeconds();
    LastKnownPlayerLocation = Player->GetActorLocation();
    LastSeenTime = Now;
    if (State == EEndlessEnemyState::Patrol)
    {
        EnterChase(Now, EEndlessEnemyState::Patrol);
    }
    else
    {
        State = EEndlessEnemyState::Search;
    }
}

void AEndlessEnemyController::Think()
{
    APawn* Self = GetPawn();
    const UWorld* World = GetWorld();
    if (!bBrainActive || !IsValid(Self) || !World)
    {
        return;
    }

    const float Now = World->GetTimeSeconds();
    const float DeltaSeconds = FMath::Max(Now - LastThinkTime, 0.01f);
    LastThinkTime = Now;

    APawn* Player = UGameplayStatics::GetPlayerPawn(this, 0);
    const bool bSeesPlayer = IsValid(Player) && CanSeePlayer(Self, Player);
    if (bSeesPlayer)
    {
        LastKnownPlayerLocation = Player->GetActorLocation();
        LastSeenTime = Now;
    }

    UpdateStuck(Self, DeltaSeconds, Now);

    switch (State)
    {
    case EEndlessEnemyState::Patrol:
        if (bSeesPlayer)
        {
            EnterChase(Now, EEndlessEnemyState::Patrol);
            TickChase(Self, Player, Now);
        }
        else
        {
            TickPatrol(Self, Now);
        }
        break;

    case EEndlessEnemyState::Chase:
        if (bSeesPlayer)
        {
            TickChase(Self, Player, Now);
        }
        else
        {
            State = EEndlessEnemyState::Search;
            NextRepathTime = 0.0f;
            ClearFocus(EAIFocusPriority::Gameplay);
            SetFacingMode(false);
            TickSearch(Self, Now);
        }
        break;

    case EEndlessEnemyState::Search:
        if (bSeesPlayer)
        {
            EnterChase(Now, EEndlessEnemyState::Search);
            TickChase(Self, Player, Now);
        }
        else if (Now - LastSeenTime > MemorySeconds)
        {
            EnterPatrol(Now);
        }
        else
        {
            TickSearch(Self, Now);
        }
        break;
    }
}

void AEndlessEnemyController::UpdateStuck(APawn* Self, const float DeltaSeconds, const float Now)
{
    const FVector Location = Self->GetActorLocation();
    const bool bMoving = GetMoveStatus() == EPathFollowingStatus::Moving;
    const float MinDistance = StuckSpeed * DeltaSeconds;

    if (bMoving && FVector::DistSquared2D(Location, LastThinkLocation) < FMath::Square(MinDistance))
    {
        StuckTime += DeltaSeconds;
    }
    else
    {
        StuckTime = 0.0f;
    }
    LastThinkLocation = Location;

    if (StuckTime >= StuckSeconds)
    {
        if (bLogDebug)
        {
            UE_LOG(LogEndlessEnemyAI, Log, TEXT("%s: stuck, re-planning."), *Self->GetName());
        }
        StopMovement();
        bHasPatrolDestination = false;
        PatrolWaitUntil = Now + 0.2f;
        NextRepathTime = 0.0f;
        StuckTime = 0.0f;
    }
}

bool AEndlessEnemyController::CanSeePlayer(const APawn* Self, APawn* Player)
{
    const FVector ToPlayer = Player->GetActorLocation() - Self->GetActorLocation();
    const bool bAlerted = State != EEndlessEnemyState::Patrol;
    const float MaxDistance = bAlerted ? LoseSightRadius : SightRadius;
    if (ToPlayer.SizeSquared() > FMath::Square(MaxDistance))
    {
        return false;
    }

    if (!bAlerted)
    {
        const FVector Forward = Self->GetActorForwardVector().GetSafeNormal2D();
        const FVector Direction = ToPlayer.GetSafeNormal2D();
        const float MinDot = FMath::Cos(FMath::DegreesToRadians(SightHalfAngle));
        if (FVector::DotProduct(Forward, Direction) < MinDot)
        {
            return false;
        }
    }

    return LineOfSightTo(Player);
}

void AEndlessEnemyController::EnterPatrol(const float Now)
{
    if (bLogDebug && GetPawn())
    {
        UE_LOG(LogEndlessEnemyAI, Log, TEXT("%s: -> Patrol"), *GetPawn()->GetName());
    }
    State = EEndlessEnemyState::Patrol;
    ClearFocus(EAIFocusPriority::Gameplay);
    SetFacingMode(false);
    StopMovement();
    bHasPatrolDestination = false;
    PatrolWaitUntil = Now + 0.5f;
}

void AEndlessEnemyController::EnterChase(const float Now, const EEndlessEnemyState PreviousState)
{
    if (bLogDebug && GetPawn())
    {
        UE_LOG(LogEndlessEnemyAI, Log, TEXT("%s: -> Chase"), *GetPawn()->GetName());
    }
    State = EEndlessEnemyState::Chase;
    bHasPatrolDestination = false;
    NextRepathTime = 0.0f;
    SetFacingMode(true);
    if (PreviousState == EEndlessEnemyState::Patrol)
    {
        NextFireTime = FMath::Max(NextFireTime, Now + ReactionTime);
    }
}

void AEndlessEnemyController::TickPatrol(APawn* Self, const float Now)
{
    // A trip that finished (or failed) leaves the move status idle: pause, then pick a new point.
    if (bHasPatrolDestination && !IsMoveActive())
    {
        bHasPatrolDestination = false;
        PatrolWaitUntil = Now + FMath::FRandRange(
            FMath::Min(PatrolWaitMin, PatrolWaitMax), FMath::Max(PatrolWaitMin, PatrolWaitMax));
        return;
    }
    if (IsMoveActive() || Now < PatrolWaitUntil)
    {
        return;
    }

    UNavigationSystemV1* NavSys = UNavigationSystemV1::GetCurrent(GetWorld());
    if (!NavSys)
    {
        PatrolWaitUntil = Now + 1.0f;
        return;
    }

    const FVector Location = Self->GetActorLocation();
    FNavLocation Best;
    bool bFound = false;
    for (int32 Attempt = 0; Attempt < 6; ++Attempt)
    {
        FNavLocation Candidate;
        if (!NavSys->GetRandomReachablePointInRadius(Location, PatrolRadius, Candidate))
        {
            continue;
        }
        Best = Candidate;
        bFound = true;
        if (FVector::DistSquared2D(Candidate.Location, Location) >= FMath::Square(MinPatrolDistance))
        {
            break;
        }
    }

    if (!bFound)
    {
        if (bLogDebug)
        {
            UE_LOG(LogEndlessEnemyAI, Warning,
                TEXT("%s: no reachable patrol point (NavMesh missing or not built yet?)."),
                *Self->GetName());
        }
        PatrolWaitUntil = Now + 1.0f;
        return;
    }

    const EPathFollowingRequestResult::Type Result = MoveToLocation(
        Best.Location, PatrolAcceptRadius, /*bStopOnOverlap*/ true, /*bUsePathfinding*/ true,
        /*bProjectDestinationToNavigation*/ false, /*bCanStrafe*/ false);

    if (Result == EPathFollowingRequestResult::RequestSuccessful)
    {
        bHasPatrolDestination = true;
    }
    else
    {
        PatrolWaitUntil = Now + 0.5f;
    }
}

void AEndlessEnemyController::TickChase(APawn* Self, APawn* Player, const float Now)
{
    SetFacingMode(true);
    SetFocus(Player);

    const float Distance = FVector::Dist(Self->GetActorLocation(), Player->GetActorLocation());
    if (Distance > PreferredDistance)
    {
        if (!IsMoveActive() || Now >= NextRepathTime)
        {
            MoveToActor(Player, PreferredDistance * 0.8f, /*bStopOnOverlap*/ true,
                /*bUsePathfinding*/ true, /*bCanStrafe*/ true);
            NextRepathTime = Now + RepathInterval;
        }
    }
    else if (IsMoveActive())
    {
        StopMovement();
    }

    TryShoot(Self, Player, Distance, Now);
}

void AEndlessEnemyController::TickSearch(APawn* Self, const float Now)
{
    const float DistanceToLastKnown = FVector::Dist2D(Self->GetActorLocation(), LastKnownPlayerLocation);
    if (DistanceToLastKnown > 150.0f && (!IsMoveActive() || Now >= NextRepathTime))
    {
        MoveToLocation(LastKnownPlayerLocation, 100.0f, /*bStopOnOverlap*/ true,
            /*bUsePathfinding*/ true, /*bProjectDestinationToNavigation*/ true, /*bCanStrafe*/ false);
        NextRepathTime = Now + RepathInterval * 2.0f;
    }
}

void AEndlessEnemyController::TryShoot(APawn* Self, const APawn* Player, const float Distance, const float Now)
{
    if (Distance > FireRange || Now < NextFireTime)
    {
        return;
    }

    const FVector Forward = Self->GetActorForwardVector().GetSafeNormal2D();
    const FVector Direction = (Player->GetActorLocation() - Self->GetActorLocation()).GetSafeNormal2D();
    if (FVector::DotProduct(Forward, Direction) < 0.85f)
    {
        return; // still turning towards the player
    }

    NextFireTime = Now + FireInterval + FMath::FRandRange(0.0f, FireIntervalVariance);
    FireShot(Self);
}

void AEndlessEnemyController::FireShot(APawn* Self)
{
    if (UFunction* ShootFunction = Self->FindFunction(ShootFunctionName))
    {
        if (ShootFunction->NumParms == 0)
        {
            Self->ProcessEvent(ShootFunction, nullptr);
        }
        else if (!bWarnedMissingShoot)
        {
            bWarnedMissingShoot = true;
            UE_LOG(LogEndlessEnemyAI, Warning,
                TEXT("%s: '%s' has parameters; it must be a parameterless function/event."),
                *Self->GetName(), *ShootFunctionName.ToString());
        }
        return;
    }

    if (!bWarnedMissingShoot)
    {
        bWarnedMissingShoot = true;
        UE_LOG(LogEndlessEnemyAI, Warning,
            TEXT("%s: no function named '%s'; broadcasting OnShootRequested instead."),
            *Self->GetName(), *ShootFunctionName.ToString());
    }
    OnShootRequested.Broadcast();
}

void AEndlessEnemyController::SetFacingMode(const bool bFaceTarget)
{
    if (ACharacter* OwnerChar = Cast<ACharacter>(GetPawn()))
    {
        OwnerChar->bUseControllerRotationYaw = bFaceTarget;
        if (UCharacterMovementComponent* Movement = OwnerChar->GetCharacterMovement())
        {
            Movement->bOrientRotationToMovement = !bFaceTarget;
        }
    }
}

bool AEndlessEnemyController::IsMoveActive() const
{
    return GetMoveStatus() != EPathFollowingStatus::Idle;
}
