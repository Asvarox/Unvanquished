/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.
Copyright (C) 2000-2009 Darklegion Development

This file is part of Daemon.

Daemon is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Daemon is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Daemon; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/

#include "g_local.h"

#define MISSILE_PRESTEP_TIME 50

/*
================
G_BounceMissile

================
*/
void G_BounceMissile( gentity_t *ent, trace_t *trace )
{
	vec3_t velocity;
	float  dot;
	int    hitTime;

	// reflect the velocity on the trace plane
	hitTime = level.previousTime + ( level.time - level.previousTime ) * trace->fraction;
	BG_EvaluateTrajectoryDelta( &ent->s.pos, hitTime, velocity );
	dot = DotProduct( velocity, trace->plane.normal );
	VectorMA( velocity, -2 * dot, trace->plane.normal, ent->s.pos.trDelta );

	if ( ent->s.eFlags & EF_BOUNCE_HALF )
	{
		VectorScale( ent->s.pos.trDelta, 0.65, ent->s.pos.trDelta );

		// check for stop
		if ( trace->plane.normal[ 2 ] > 0.2 && VectorLength( ent->s.pos.trDelta ) < 40 )
		{
			G_SetOrigin( ent, trace->endpos );
			return;
		}
	}

	VectorAdd( ent->r.currentOrigin, trace->plane.normal, ent->r.currentOrigin );
	VectorCopy( ent->r.currentOrigin, ent->s.pos.trBase );
	ent->s.pos.trTime = level.time;
}

/*
================
G_MissileTimePowerReduce

Reduce the power of e.g. a luciball relative to time spent travelling.
================
*/
typedef enum {
	PR_INVERSE_SQUARE, // params: 'full power' time, 'half-life' time
	                   // (reality: starts falling off before so that we approximate a smooth curve)
	PR_COSINE,         // params: lifetime, unused (but >0)
	PR_END             // unused; here so that we can have the comma above for C89
} powerReduce_t;

static float G_MissileTimePowerReduce( gentity_t *self, int fullPower, int halfLife, powerReduce_t type )
{
	int lifetime = level.time - self->creationTime;
	float travelled;
	float divider = 1;

	// allow disabling via the half-life setting
	if ( halfLife < 1 )
	{
		return 1.0f;
	}

	switch ( type )
	{
	case PR_INVERSE_SQUARE:
		travelled = lifetime + fullPower - halfLife;
		if ( travelled > halfLife * 1.25 ) // approx. point at which the two graphs meet
		{
			divider = Q_rsqrt( travelled / halfLife );
		}
		else if ( travelled >= 0 )
		{
			divider = travelled / halfLife;
			divider = cos( divider * divider / 3.375 );
		}
		break;

	case PR_COSINE:
		// curve is from -halfLife to fullPower, with a quarter-cycle being 0…fullPower
		travelled = lifetime - halfLife;
		divider = cos( travelled * M_PI / 2.0f / ( fullPower + 1 - halfLife ) );
		divider = MAX( 0.0f, divider );
		break;

	case PR_END:
		break;
	}

	return divider;
}

/*
================
G_DoMissileTimePowerReduce

Called on missile explosion or impact if the missile is otherwise not specially handled
================
*/
static float G_DoMissileTimePowerReduce( gentity_t *ent )
{
	if ( !strcmp( ent->classname, "lcannon" ) )
	{
		return G_MissileTimePowerReduce( ent, g_luciFullPowerTime.integer,
		                                      g_luciHalfLifeTime.integer,
		                                      PR_INVERSE_SQUARE );
	}
	else if ( !strcmp( ent->classname, "pulse" ) )
	{
		return G_MissileTimePowerReduce( ent, g_pulseFullPowerTime.integer,
		                                      g_pulseHalfLifeTime.integer,
		                                      PR_INVERSE_SQUARE );
	}
	else if ( !strcmp( ent->classname, "flame" ) )
	{
		return G_MissileTimePowerReduce( ent, FLAMER_LIFETIME,
		                                      g_flameFadeout.integer ? ( FLAMER_LIFETIME / 5 ) : 0,
		                                      PR_COSINE );
	}

	return 1.0f;
}

/*
================
G_ExplodeMissile

Explode a missile without an impact
================
*/
void G_ExplodeMissile( gentity_t *ent )
{
	vec3_t dir;
	vec3_t origin;

	BG_EvaluateTrajectory( &ent->s.pos, level.time, origin );
	SnapVector( origin );
	G_SetOrigin( ent, origin );

	// we don't have a valid direction, so just point straight up
	dir[ 0 ] = dir[ 1 ] = 0;
	dir[ 2 ] = 1;

	ent->s.eType = ET_GENERAL;

	if ( ent->s.weapon != WP_LOCKBLOB_LAUNCHER &&
	     ent->s.weapon != WP_FLAMER )
	{
		G_AddEvent( ent, EV_MISSILE_MISS, DirToByte( dir ) );
	}

	ent->freeAfterEvent = qtrue;

	// splash damage
	if ( ent->splashDamage )
	{
		G_RadiusDamage( ent->r.currentOrigin, ent->parent,
		                ent->splashDamage * G_DoMissileTimePowerReduce( ent ),
		                ent->splashRadius, ent, ent->splashMethodOfDeath );
	}

	trap_LinkEntity( ent );
}

/*
================
G_MissileImpact

================
*/
void G_MissileImpact( gentity_t *ent, trace_t *trace )
{
	gentity_t *other, *attacker;
	qboolean  returnAfterDamage = qfalse;
	vec3_t    dir;
	float     power;

	other = &g_entities[ trace->entityNum ];
	attacker = &g_entities[ ent->r.ownerNum ];

	// check for bounce
	if ( !other->takedamage &&
	     ( ent->s.eFlags & ( EF_BOUNCE | EF_BOUNCE_HALF ) ) )
	{
		G_BounceMissile( ent, trace );

		//only play a sound if requested
		if ( !( ent->s.eFlags & EF_NO_BOUNCE_SOUND ) )
		{
			G_AddEvent( ent, EV_GRENADE_BOUNCE, 0 );
		}

		return;
	}

	if ( !strcmp( ent->classname, "grenade" ) )
	{
		//grenade doesn't explode on impact
		G_BounceMissile( ent, trace );

		//only play a sound if requested
		if ( !( ent->s.eFlags & EF_NO_BOUNCE_SOUND ) )
		{
			G_AddEvent( ent, EV_GRENADE_BOUNCE, 0 );
		}

		return;
	}
	else if ( !strcmp( ent->classname, "lockblob" ) )
	{
		if ( other->client && other->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
		{
			other->client->ps.stats[ STAT_STATE ] |= SS_BLOBLOCKED;
			other->client->lastLockTime = level.time;
			AngleVectors( other->client->ps.viewangles, dir, NULL, NULL );
			other->client->ps.stats[ STAT_VIEWLOCK ] = DirToByte( dir );
		}
	}
	else if ( !strcmp( ent->classname, "slowblob" ) )
	{
		if ( other->client && other->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
		{
			other->client->ps.stats[ STAT_STATE ] |= SS_SLOWLOCKED;
			other->client->lastSlowTime = level.time;
			AngleVectors( other->client->ps.viewangles, dir, NULL, NULL );
			other->client->ps.stats[ STAT_VIEWLOCK ] = DirToByte( dir );
		}
	}
	else if ( !strcmp( ent->classname, "hive" ) )
	{
		if ( other->s.eType == ET_BUILDABLE && other->s.modelindex == BA_A_HIVE )
		{
			if ( !ent->parent )
			{
				G_Printf( S_WARNING "hive entity has no parent in G_MissileImpact\n" );
			}
			else
			{
				ent->parent->active = qfalse;
			}

			G_FreeEntity( ent );
			return;
		}
		else
		{
			//prevent collision with the client when returning
			ent->r.ownerNum = other->s.number;

			ent->think = G_ExplodeMissile;
			ent->nextthink = level.time + FRAMETIME;

			//only damage humans
			if ( other->client && other->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS )
			{
				returnAfterDamage = qtrue;
			}
			else
			{
				return;
			}
		}
	}

	power = G_DoMissileTimePowerReduce( ent );

	// impact damage
	if ( other->takedamage )
	{
		// FIXME: wrong damage direction?
		if ( ent->damage )
		{
			vec3_t dir;

			BG_EvaluateTrajectoryDelta( &ent->s.pos, level.time, dir );

			if ( VectorNormalize( dir ) == 0 )
			{
				dir[ 2 ] = 1; // stepped on a grenade
			}

			G_Damage( other, ent, attacker, dir, ent->s.origin, ent->damage * power,
			          DAMAGE_NO_LOCDAMAGE, ent->methodOfDeath );
		}
	}

	if ( returnAfterDamage )
	{
		return;
	}

	// is it cheaper in bandwidth to just remove this ent and create a new
	// one, rather than changing the missile into the explosion?

	if ( other->takedamage &&
	     ( other->s.eType == ET_PLAYER || other->s.eType == ET_BUILDABLE ) )
	{
		G_AddEvent( ent, EV_MISSILE_HIT, DirToByte( trace->plane.normal ) );
		ent->s.otherEntityNum = other->s.number;
	}
	else if ( trace->surfaceFlags & SURF_METAL )
	{
		G_AddEvent( ent, EV_MISSILE_MISS_METAL, DirToByte( trace->plane.normal ) );
	}
	else
	{
		G_AddEvent( ent, EV_MISSILE_MISS, DirToByte( trace->plane.normal ) );
	}

	ent->freeAfterEvent = qtrue;

	// change over to a general entity right at the point of impact
	ent->s.eType = ET_GENERAL;

	SnapVectorTowards( trace->endpos, ent->s.pos.trBase );  // save net bandwidth

	G_SetOrigin( ent, trace->endpos );

	// splash damage (doesn't apply to person directly hit)
	if ( ent->splashDamage )
	{
		G_RadiusDamage( trace->endpos, ent->parent, ent->splashDamage * power, ent->splashRadius,
		                other, ent->splashMethodOfDeath );
	}

	trap_LinkEntity( ent );
}

/*
================
G_RunMissile

================
*/
void G_RunMissile( gentity_t *ent )
{
	vec3_t   origin;
	trace_t  tr;
	int      passent;
	qboolean impact = qfalse;

	// get current position
	BG_EvaluateTrajectory( &ent->s.pos, level.time, origin );

	// ignore interactions with the missile owner
	passent = ent->r.ownerNum;

	// general trace to see if we hit anything at all
	trap_Trace( &tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs,
	            origin, passent, ent->clipmask );

	if ( tr.startsolid || tr.allsolid )
	{
		tr.fraction = 0.0f;
		VectorCopy( ent->r.currentOrigin, tr.endpos );
	}

	if ( tr.fraction < 1.0f )
	{
		if ( !ent->pointAgainstWorld || (tr.contents & CONTENTS_BODY) )
		{
			// We hit an entity or we don't care
			impact = qtrue;
		}
		else
		{
			trap_Trace( &tr, ent->r.currentOrigin, NULL, NULL, origin,
			            passent, ent->clipmask );

			if ( tr.fraction < 1.0f )
			{
				// Hit the world with point trace
				impact = qtrue;
			}
			else
			{
				if ( tr.contents & CONTENTS_BODY )
				{
					// Hit an entity
					impact = qtrue;
				}
				else
				{
					trap_Trace( &tr, ent->r.currentOrigin, ent->r.mins, ent->r.maxs,
					            origin, passent, CONTENTS_BODY );

					if ( tr.fraction < 1.0f )
					{
						impact = qtrue;
					}
				}
			}
		}
	}

	VectorCopy( tr.endpos, ent->r.currentOrigin );

	if ( impact )
	{
		if ( tr.surfaceFlags & SURF_NOIMPACT )
		{
			// Never explode or bounce on sky
			G_FreeEntity( ent );
			return;
		}

		G_MissileImpact( ent, &tr );

		if ( ent->s.eType != ET_MISSILE )
		{
			return; // exploded
		}
	}

	ent->r.contents = CONTENTS_SOLID; //trick trap_LinkEntity into...
	trap_LinkEntity( ent );
	ent->r.contents = 0; //...encoding bbox information

	if ( ent->flightSplashDamage )
	{
		G_RadiusDamage( tr.endpos, ent->parent,
		                ent->flightSplashDamage * G_DoMissileTimePowerReduce( ent ), ent->splashRadius,
		                ent->parent, ent->splashMethodOfDeath );
	}

	// check think function after bouncing
	G_RunThink( ent );
}

//=============================================================================

/*
=================
fire_flamer

=================
*/
gentity_t *fire_flamer( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;
	vec3_t    pvel;

	bolt = G_NewEntity();
	bolt->classname = "flame";
	bolt->pointAgainstWorld = qfalse;
	bolt->nextthink = level.time + FLAMER_LIFETIME;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_FLAMER;
	bolt->s.generic1 = self->s.generic1; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = FLAMER_DMG;
	bolt->flightSplashDamage = FLAMER_FLIGHTSPLASHDAMAGE;
	bolt->splashDamage = FLAMER_SPLASHDAMAGE;
	bolt->splashRadius = FLAMER_RADIUS;
	bolt->methodOfDeath = MOD_FLAMER;
	bolt->splashMethodOfDeath = MOD_FLAMER_SPLASH;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;
	bolt->r.mins[ 0 ] = bolt->r.mins[ 1 ] = bolt->r.mins[ 2 ] = -FLAMER_SIZE;
	bolt->r.maxs[ 0 ] = bolt->r.maxs[ 1 ] = bolt->r.maxs[ 2 ] = FLAMER_SIZE;

	bolt->s.pos.trType = TR_LINEAR;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( self->client->ps.velocity, FLAMER_LAG, pvel );
	VectorMA( pvel, FLAMER_SPEED, dir, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth

	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}

//=============================================================================

/*
=================
fire_blaster

=================
*/
gentity_t *fire_blaster( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "blaster";
	bolt->pointAgainstWorld = qtrue;
	bolt->nextthink = level.time + 10000;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_BLASTER;
	bolt->s.generic1 = self->s.generic1; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = BLASTER_DMG;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->methodOfDeath = MOD_BLASTER;
	bolt->splashMethodOfDeath = MOD_BLASTER;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;
	bolt->r.mins[ 0 ] = bolt->r.mins[ 1 ] = bolt->r.mins[ 2 ] = -BLASTER_SIZE;
	bolt->r.maxs[ 0 ] = bolt->r.maxs[ 1 ] = bolt->r.maxs[ 2 ] = BLASTER_SIZE;

	bolt->s.pos.trType = TR_LINEAR;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, BLASTER_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth

	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}

//=============================================================================

/*
=================
fire_pulseRifle

=================
*/
gentity_t *fire_pulseRifle( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "pulse";
	bolt->pointAgainstWorld = qtrue;
	bolt->nextthink = level.time + 10000;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_PULSE_RIFLE;
	bolt->s.generic1 = self->s.generic1; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = PRIFLE_DMG;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->methodOfDeath = MOD_PRIFLE;
	bolt->splashMethodOfDeath = MOD_PRIFLE;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;
	bolt->r.mins[ 0 ] = bolt->r.mins[ 1 ] = bolt->r.mins[ 2 ] = -PRIFLE_SIZE;
	bolt->r.maxs[ 0 ] = bolt->r.maxs[ 1 ] = bolt->r.maxs[ 2 ] = PRIFLE_SIZE;

	bolt->s.pos.trType = TR_LINEAR;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, PRIFLE_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth

	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}

//=============================================================================

/*
=================
fire_luciferCannon

=================
*/
gentity_t *fire_luciferCannon( gentity_t *self, vec3_t start, vec3_t dir,
                               int damage, int radius, int speed )
{
	gentity_t *bolt;
	float     charge;

	bolt = G_NewEntity();
	bolt->classname = "lcannon";
	bolt->pointAgainstWorld = qtrue;

	if ( damage == LCANNON_DAMAGE )
	{
		bolt->nextthink = level.time;
	}
	else
	{
		bolt->nextthink = level.time + 10000;
	}

	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_LUCIFER_CANNON;
	bolt->s.generic1 = self->s.generic1; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = damage;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = damage / 2;
	bolt->splashRadius = radius;
	bolt->methodOfDeath = MOD_LCANNON;
	bolt->splashMethodOfDeath = MOD_LCANNON_SPLASH;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;

	// Give the missile a small bounding box
	bolt->r.mins[ 0 ] = bolt->r.mins[ 1 ] = bolt->r.mins[ 2 ] =
	    -LCANNON_SIZE;
	bolt->r.maxs[ 0 ] = bolt->r.maxs[ 1 ] = bolt->r.maxs[ 2 ] =
	    -bolt->r.mins[ 0 ];

	// Pass the missile charge through
	charge = ( float )( damage - LCANNON_SECONDARY_DAMAGE ) / LCANNON_DAMAGE;
	bolt->s.torsoAnim = charge * 255;

	if ( bolt->s.torsoAnim < 0 )
	{
		bolt->s.torsoAnim = 0;
	}

	bolt->s.pos.trType = TR_LINEAR;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, speed, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth

	VectorCopy( start, bolt->r.currentOrigin );

//	Com_Printf("Luciball power = %d, speed = %d/s.\n", damage, speed);
	return bolt;
}

/*
=================
launch_grenade

=================
*/
gentity_t *launch_grenade( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "grenade";
	bolt->pointAgainstWorld = qfalse;
	bolt->nextthink = level.time + 5000;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_GRENADE;
	bolt->s.eFlags = EF_BOUNCE_HALF;
	bolt->s.generic1 = WPM_PRIMARY; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = GRENADE_DAMAGE;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = GRENADE_DAMAGE;
	bolt->splashRadius = GRENADE_RANGE;
	bolt->methodOfDeath = MOD_GRENADE;
	bolt->splashMethodOfDeath = MOD_GRENADE;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;
	bolt->r.mins[ 0 ] = bolt->r.mins[ 1 ] = bolt->r.mins[ 2 ] = -3.0f;
	bolt->r.maxs[ 0 ] = bolt->r.maxs[ 1 ] = bolt->r.maxs[ 2 ] = 3.0f;
	bolt->s.time = level.time;

	bolt->s.pos.trType = TR_GRAVITY;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, GRENADE_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth

	VectorCopy( start, bolt->r.currentOrigin );

	trap_SendServerCommand( self - g_entities, "vcommand grenade" );

	return bolt;
}

//=============================================================================

/*
================
AHive_SearchAndDestroy

Adjust the trajectory to point towards the target
================
*/
void AHive_SearchAndDestroy( gentity_t *self )
{
	vec3_t    dir;
	trace_t   tr;
	gentity_t *ent;
	int       i;
	float     d, nearest;

	if ( level.time > self->timestamp )
	{
		VectorCopy( self->r.currentOrigin, self->s.pos.trBase );
		self->s.pos.trType = TR_STATIONARY;
		self->s.pos.trTime = level.time;

		self->think = G_ExplodeMissile;
		self->nextthink = level.time + 50;
		self->parent->active = qfalse; //allow the parent to start again
		return;
	}

	nearest = DistanceSquared( self->r.currentOrigin, self->target->r.currentOrigin );

	//find the closest human
	for ( i = 0; i < MAX_CLIENTS; i++ )
	{
		ent = &g_entities[ i ];

		if ( ent->flags & FL_NOTARGET )
		{
			continue;
		}

		if ( ent->client &&
		     ent->health > 0 &&
		     ent->client->ps.stats[ STAT_TEAM ] == TEAM_HUMANS &&
		     nearest > ( d = DistanceSquared( ent->r.currentOrigin, self->r.currentOrigin ) ) )
		{
			trap_Trace( &tr, self->r.currentOrigin, self->r.mins, self->r.maxs,
			            ent->r.currentOrigin, self->r.ownerNum, self->clipmask );

			if ( tr.entityNum != ENTITYNUM_WORLD )
			{
				nearest = d;
				self->target = ent;
			}
		}
	}

	VectorSubtract( self->target->r.currentOrigin, self->r.currentOrigin, dir );
	VectorNormalize( dir );

	//change direction towards the player
	VectorScale( dir, HIVE_SPEED, self->s.pos.trDelta );
	SnapVector( self->s.pos.trDelta );  // save net bandwidth
	VectorCopy( self->r.currentOrigin, self->s.pos.trBase );
	self->s.pos.trTime = level.time;

	self->nextthink = level.time + HIVE_DIR_CHANGE_PERIOD;
}

/*
=================
fire_hive
=================
*/
gentity_t *fire_hive( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "hive";
	bolt->pointAgainstWorld = qfalse;
	bolt->nextthink = level.time + HIVE_DIR_CHANGE_PERIOD;
	bolt->think = AHive_SearchAndDestroy;
	bolt->s.eType = ET_MISSILE;
	bolt->s.eFlags |= EF_BOUNCE | EF_NO_BOUNCE_SOUND;
	bolt->s.weapon = WP_HIVE;
	bolt->s.generic1 = WPM_PRIMARY; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = HIVE_DMG;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->methodOfDeath = MOD_SWARM;
	bolt->clipmask = MASK_SHOT;
	bolt->target = self->target;
	bolt->timestamp = level.time + HIVE_LIFETIME;

	bolt->s.pos.trType = TR_LINEAR;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, HIVE_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth
	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}

//=============================================================================

/*
=================
fire_lockblob
=================
*/
gentity_t *fire_lockblob( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "lockblob";
	bolt->pointAgainstWorld = qtrue;
	bolt->nextthink = level.time + 15000;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_LOCKBLOB_LAUNCHER;
	bolt->s.generic1 = WPM_PRIMARY; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = 0;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->methodOfDeath = MOD_UNKNOWN; //doesn't do damage so will never kill
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;

	bolt->s.pos.trType = TR_LINEAR;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, LOCKBLOB_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth
	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}

/*
=================
fire_slowBlob
=================
*/
gentity_t *fire_slowBlob( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "slowblob";
	bolt->pointAgainstWorld = qtrue;
	bolt->nextthink = level.time + 15000;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_ABUILD2;
	bolt->s.generic1 = self->s.generic1; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = ABUILDER_BLOB_DMG;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = 0;
	bolt->splashRadius = 0;
	bolt->methodOfDeath = MOD_SLOWBLOB;
	bolt->splashMethodOfDeath = MOD_SLOWBLOB;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;

	bolt->s.pos.trType = TR_GRAVITY;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, ABUILDER_BLOB_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth
	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}

/*
=================
fire_bounceBall
=================
*/
gentity_t *fire_bounceBall( gentity_t *self, vec3_t start, vec3_t dir )
{
	gentity_t *bolt;

	bolt = G_NewEntity();
	bolt->classname = "bounceball";
	bolt->pointAgainstWorld = qtrue;
	bolt->nextthink = level.time + 3000;
	bolt->think = G_ExplodeMissile;
	bolt->s.eType = ET_MISSILE;
	bolt->s.weapon = WP_ALEVEL3_UPG;
	bolt->s.generic1 = self->s.generic1; //weaponMode
	bolt->r.ownerNum = self->s.number;
	bolt->parent = self;
	bolt->damage = LEVEL3_BOUNCEBALL_DMG;
	bolt->flightSplashDamage = 0;
	bolt->splashDamage = LEVEL3_BOUNCEBALL_DMG;
	bolt->splashRadius = LEVEL3_BOUNCEBALL_RADIUS;
	bolt->methodOfDeath = MOD_LEVEL3_BOUNCEBALL;
	bolt->splashMethodOfDeath = MOD_LEVEL3_BOUNCEBALL;
	bolt->clipmask = MASK_SHOT;
	bolt->target = NULL;

	bolt->s.pos.trType = TR_GRAVITY;
	bolt->s.pos.trTime = level.time - MISSILE_PRESTEP_TIME; // move a bit on the very first frame
	VectorCopy( start, bolt->s.pos.trBase );
	VectorScale( dir, LEVEL3_BOUNCEBALL_SPEED, bolt->s.pos.trDelta );
	SnapVector( bolt->s.pos.trDelta );  // save net bandwidth
	VectorCopy( start, bolt->r.currentOrigin );

	return bolt;
}
