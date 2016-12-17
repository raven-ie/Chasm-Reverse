#include <cstring>

#include <matrix.hpp>

#include "../game_constants.hpp"
#include "../math_utils.hpp"
#include "../particles.hpp"
#include "a_code.hpp"
#include "collisions.hpp"
#include "monster.hpp"
#include "player.hpp"

#include "map.hpp"

namespace PanzerChasm
{

static const float g_commands_coords_scale= 1.0f / 256.0f;

template<class Wall>
static m_Vec3 GetNormalForWall( const Wall& wall )
{
	m_Vec3 n( wall.vert_pos[0].y - wall.vert_pos[1].y, wall.vert_pos[1].x - wall.vert_pos[0].x, 0.0f );
	return n / n.xy().Length();
}

Map::Rocket::Rocket(
	const Messages::EntityId in_rocket_id,
	const Messages::EntityId in_owner_id,
	const unsigned char in_rocket_type_id,
	const m_Vec3& in_start_point,
	const m_Vec3& in_normalized_direction,
	const Time in_start_time )
	: start_time( in_start_time )
	, start_point( in_start_point )
	, normalized_direction( in_normalized_direction )
	, rocket_id( in_rocket_id )
	, owner_id( in_owner_id )
	, rocket_type_id( in_rocket_type_id )
	, previous_position( in_start_point )
	, track_length( 0.0f )
{}

bool Map::Rocket::HasInfiniteSpeed( const GameResources& game_resources ) const
{
	PC_ASSERT( rocket_type_id < game_resources.rockets_description.size() );
	return game_resources.rockets_description[ rocket_type_id ].model_file_name[0] == '\0';
}

template<class Func>
void Map::ProcessElementLinks(
	const MapData::IndexElement::Type element_type,
	const unsigned int index,
	const Func& func )
{
	for( const MapData::Link& link : map_data_->links )
	{
		const MapData::IndexElement& index_element= map_data_->map_index[ link.x + link.y * MapData::c_map_size ];

		if( index_element.type == element_type && index_element.index == index )
			func( link );
	}
}

Map::Map(
	const MapDataConstPtr& map_data,
	const GameResourcesConstPtr& game_resources,
	const Time map_start_time,
	MapEndCallback map_end_callback )
	: map_data_(map_data)
	, game_resources_(game_resources)
	, map_end_callback_( std::move( map_end_callback ) )
	, random_generator_( std::make_shared<LongRand>() )
{
	PC_ASSERT( map_data_ != nullptr );
	PC_ASSERT( game_resources_ != nullptr );

	std::memset( wind_field_, 0, sizeof(wind_field_) );

	procedures_.resize( map_data_->procedures.size() );
	for( unsigned int p= 0u; p < procedures_.size(); p++ )
	{
		if( map_data_->procedures[p].locked )
			procedures_[p].locked= true;
	}

	dynamic_walls_.resize( map_data_->dynamic_walls.size() );
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		dynamic_walls_[w].texture_id= map_data_->dynamic_walls[w].texture_id;
	}

	static_models_.resize( map_data_->static_models.size() );
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const MapData::StaticModel& in_model= map_data_->static_models[m];
		StaticModel& out_model= static_models_[m];

		out_model.model_id= in_model.model_id;

		const MapData::ModelDescription* const model_description=
			in_model.model_id < map_data_->models_description.size()
				? &map_data_->models_description[ in_model.model_id ] : nullptr;

		out_model.health= model_description == nullptr ? 0 : model_description->break_limit;

		out_model.pos= m_Vec3( in_model.pos, 0.0f );
		out_model.angle= in_model.angle;
		out_model.baze_z= 0.0f;

		if( model_description != nullptr &&
			static_cast<ACode>(model_description->ac) == ACode::Switch )
			out_model.animation_state= StaticModel::AnimationState::SingleFrame;
		else
			out_model.animation_state= StaticModel::AnimationState::Animation;

		out_model.animation_start_time= map_start_time;
		out_model.animation_start_frame= 0u;
	}

	items_.resize( map_data_->items.size() );
	for( unsigned int i= 0u; i < items_.size(); i++ )
	{
		const MapData::Item& in_item= map_data_->items[i];
		Item& out_item= items_[i];

		out_item.item_id= in_item.item_id;
		out_item.pos= m_Vec3( in_item.pos, 0.0f );
		out_item.picked_up= false;
	}

	// Pull up items, which placed atop of models
	for( Item& item : items_ )
	{
		item.pos.z= GetFloorLevel( item.pos.xy() );
	}
	for( StaticModel& model : static_models_ )
	{
		if( model.model_id >= map_data_->models_description.size() )
			continue;

		if( map_data_->models_description[ model.model_id ].ac == 0u )
			continue;

		model.pos.z= model.baze_z= GetFloorLevel( model.pos.xy() );
	}

	// Spawn monsters
	for( const MapData::Monster& map_monster : map_data_->monsters )
	{
		// Skip players
		if( map_monster.monster_id == 0u )
			continue;

		// TODO - check difficulty flags
		monsters_[ GetNextMonsterId() ]=
			MonsterPtr(
				new Monster(
					map_monster,
					GetFloorLevel( map_monster.pos ),
					game_resources_,
					random_generator_,
					map_start_time ) );
	}
}

Map::~Map()
{}

void Map::SpawnPlayer( const PlayerPtr& player )
{
	PC_ASSERT( player != nullptr );

	unsigned int min_spawn_number= ~0u;
	const MapData::Monster* spawn_with_min_number= nullptr;

	for( const MapData::Monster& monster : map_data_->monsters )
	{
		if( monster.difficulty_flags < min_spawn_number )
		{
			min_spawn_number= monster.difficulty_flags;
			spawn_with_min_number= &monster;
		}
	}

	if( spawn_with_min_number != nullptr )
	{
		player->Teleport(
			m_Vec3(
				spawn_with_min_number->pos,
				GetFloorLevel( spawn_with_min_number->pos, GameConstants::player_radius ) ),
			spawn_with_min_number->angle );
	}
	else
		player->SetPosition( m_Vec3( 0.0f, 0.0f, 4.0f ) );

	player->SetRandomGenerator( random_generator_ );
	player->ResetActivatedProcedure();

	const Messages::EntityId player_id= GetNextMonsterId();

	players_.emplace( player_id, player );
	monsters_.emplace( player_id, player );
}

void Map::Shoot(
	const Messages::EntityId owner_id,
	const unsigned int rocket_id,
	const m_Vec3& from,
	const m_Vec3& normalized_direction,
	const Time current_time )
{
	rockets_.emplace_back( next_rocket_id_, owner_id, rocket_id, from, normalized_direction, current_time );
	next_rocket_id_++;

	const Rocket& rocket= rockets_.back();
	if( !rocket.HasInfiniteSpeed( *game_resources_ ) )
	{
		Messages::RocketBirth message;

		message.rocket_id= rocket.rocket_id;
		message.rocket_type= rocket.rocket_type_id;

		PositionToMessagePosition( rocket.start_point, message.xyz );

		float angle[2];
		VecToAngles( rocket.normalized_direction, angle );
		for( unsigned int j= 0u; j < 2u; j++ )
			message.angle[j]= AngleToMessageAngle( angle[j] );

		rockets_birth_messages_.emplace_back( message );
	}
}

m_Vec3 Map::CollideWithMap( const m_Vec3 in_pos, const float height, const float radius, bool& out_on_floor ) const
{
	m_Vec2 pos= in_pos.xy();
	out_on_floor= false;

	const float z_bottom= in_pos.z;
	const float z_top= z_bottom + height;
	float new_z= in_pos.z;

	// Static walls
	for( const MapData::Wall& wall : map_data_->static_walls )
	{
		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ wall.texture_id ];
		if( tex.gso[0] )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, radius,
				new_pos ) )
		{
			pos= new_pos;
		}
	}

	// Dynamic walls
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		const DynamicWall& wall= dynamic_walls_[w];
		const MapData::Wall& map_wall= map_data_->dynamic_walls[w];

		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ map_wall.texture_id ];
		if( tex.gso[0] )
			continue;

		if( z_top < wall.z || z_bottom > wall.z + GameConstants::walls_height )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, radius,
				new_pos ) )
		{
			pos= new_pos;
		}
	}

	// Models
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const StaticModel& model= static_models_[m];
		if( model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];
		if( model_description.radius <= 0.0f )
			continue;

		const Model& model_geometry= map_data_->models[ model.model_id ];

		const float model_z_min= model_geometry.z_min + model.pos.z;
		const float model_z_max= model_geometry.z_max + model.pos.z;
		if( z_top < model_z_min || z_bottom > model_z_max )
			continue;

		const float min_distance= radius + model_description.radius;

		const m_Vec2 vec_to_pos= pos - model.pos.xy();
		const float square_distance= vec_to_pos.SquareLength();

		if( square_distance <= min_distance * min_distance )
		{
			// Pull up or down player.
			if( model_geometry.z_max - z_bottom <= GameConstants::player_z_pull_distance )
			{
				new_z= std::max( new_z, model_z_max );
				out_on_floor= true;
			}
			else if( z_top - model_geometry.z_min <= GameConstants::player_z_pull_distance )
				new_z= std::min( new_z, model_z_min - height );
			// Push sideways.
			else
				pos= model.pos.xy() + vec_to_pos * ( min_distance / std::sqrt( square_distance ) );
		}
	}

	if( new_z <= 0.0f )
	{
		out_on_floor= true;
		new_z= 0.0f;
	}

	return m_Vec3( pos, new_z );
}

bool Map::CanSee( const m_Vec3& from, const m_Vec3& to ) const
{
	if( from == to )
		return true;

	m_Vec3 direction= to - from;
	direction.Normalize();

	const HitResult hit_result= ProcessShot( from, direction, 0u );

	return
		hit_result.object_type == HitResult::ObjectType::None ||
		hit_result.object_type == HitResult::ObjectType::Monster;
}

const Map::PlayersContainer& Map::GetPlayers() const
{
	return players_;
}

void Map::ProcessPlayerPosition(
	const Time current_time,
	Player& player,
	MessagesSender& messages_sender )
{
	const int player_x= static_cast<int>( std::floor( player.Position().x ) );
	const int player_y= static_cast<int>( std::floor( player.Position().y ) );
	if( player_x < 0 || player_y < 0 ||
		player_x >= int(MapData::c_map_size) ||
		player_y >= int(MapData::c_map_size) )
		return;

	// Process floors
	for( int x= std::max( 0, int(player_x) - 2); x < std::min( int(MapData::c_map_size), int(player_x) + 2 ); x++ )
	for( int y= std::max( 0, int(player_y) - 2); y < std::min( int(MapData::c_map_size), int(player_y) + 2 ); y++ )
	{
		// TODO - select correct player radius for floor collisions.
		if( !CircleIntersectsWithSquare(
				player.Position().xy(), GameConstants::player_radius, x, y ) )
			continue;

		for( const MapData::Link& link : map_data_->links )
		{
			if( link.type == MapData::Link::Floor && link.x == x && link.y == y )
				TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
		}
	}

	const m_Vec2 pos= player.Position().xy();
	const float z_bottom= player.Position().z;
	const float z_top= player.Position().z + GameConstants::player_height;

	// Static walls lonks.
	for( const MapData::Wall& wall : map_data_->static_walls )
	{
		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ wall.texture_id ];
		if( tex.gso[0] )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, GameConstants::player_interact_radius,
				new_pos ) )
		{
			ProcessElementLinks(
				MapData::IndexElement::StaticWall,
				&wall - map_data_->static_walls.data(),
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Link_ )
						TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
				} );
		}
	}

	// Dynamic walls links.
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		const DynamicWall& wall= dynamic_walls_[w];
		const MapData::Wall& map_wall= map_data_->dynamic_walls[w];

		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ map_wall.texture_id ];
		if( tex.gso[0] )
			continue;

		if( z_top < wall.z || z_bottom > wall.z + GameConstants::walls_height )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, GameConstants::player_interact_radius,
				new_pos ) )
		{
			ProcessElementLinks(
				MapData::IndexElement::DynamicWall,
				w,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Link_ )
						TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
				} );
		}
	}

	// Models links.
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const StaticModel& model= static_models_[m];
		if( model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];

		const Model& model_geometry= map_data_->models[ model.model_id ];

		const float model_z_min= model_geometry.z_min + model.pos.z;
		const float model_z_max= model_geometry.z_max + model.pos.z;
		if( z_top < model_z_min || z_bottom > model_z_max )
			continue;

		const float min_distance= GameConstants::player_interact_radius + model_description.radius;

		const m_Vec2 vec_to_player_pos= pos - model.pos.xy();
		const float square_distance= vec_to_player_pos.SquareLength();

		if( square_distance <= min_distance * min_distance )
		{
			// Links must work for zero radius
			ProcessElementLinks(
				MapData::IndexElement::StaticModel,
				m,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Link_ )
						TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
				} );
		}
	}

	// Process "special" models.
	// Pick-up keys.
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		StaticModel& model= static_models_[m];
		const MapData::StaticModel& map_model= map_data_->static_models[m];

		if( model.pos.z < 0.0f ||
			map_model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ map_model.model_id ];

		const ACode a_code= static_cast<ACode>( model_description.ac );
		if( a_code >= ACode::RedKey && a_code <= ACode::BlueKey )
		{
			const m_Vec2 vec_to_player_pos= pos - model.pos.xy();
			const float square_distance= vec_to_player_pos.SquareLength();
			const float min_length= GameConstants::player_radius + model_description.radius;
			if( square_distance <= min_length * min_length )
			{
				model.pos.z= -2.0f; // HACK. TODO - hide models
				if( a_code == ACode::RedKey )
					player.GiveRedKey();
				if( a_code == ACode::GreenKey )
					player.GiveGreenKey();
				if( a_code == ACode::BlueKey )
					player.GiveBlueKey();

				ProcessElementLinks(
					MapData::IndexElement::StaticModel,
					m,
					[&]( const MapData::Link& link )
					{
						if( link.type == MapData::Link::Link_ )
							TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					} );
			}
		}
	}

	//Process items
	for( Item& item : items_ )
	{
		if( item.picked_up )
			continue;

		const float square_distance= ( item.pos.xy() - pos ).SquareLength();
		if( square_distance <= GameConstants::player_interact_radius * GameConstants::player_interact_radius )
		{
			item.picked_up= player.TryPickupItem( item.item_id );
			if( item.picked_up )
			{
				// Try activate item links
				ProcessElementLinks(
					MapData::IndexElement::Item,
					&item - items_.data(),
					[&]( const MapData::Link& link )
					{
						if( link.type == MapData::Link::Link_ )
							TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					} );
			}
		}
	}
}

void Map::Tick( const Time current_time, const Time last_tick_delta )
{
	// Update state of procedures
	for( unsigned int p= 0u; p < procedures_.size(); p++ )
	{
		const MapData::Procedure& procedure= map_data_->procedures[p];
		ProcedureState& procedure_state= procedures_[p];

		const Time time_since_last_state_change= current_time - procedure_state.last_state_change_time;
		const float new_stage=
			procedure.speed > 0.0f
				? ( time_since_last_state_change.ToSeconds() * procedure.speed / 10.0f )
				: 1.0f;

		// Check map end
		if( procedure_state.movement_state != ProcedureState::MovementState::None &&
			procedure.end_delay_s > 0.0f &&
			time_since_last_state_change.ToSeconds() >= procedure.end_delay_s )
			map_end_triggered_= true;

		switch( procedure_state.movement_state )
		{
		case ProcedureState::MovementState::None:
			break;

		case ProcedureState::MovementState::StartWait:
			if( time_since_last_state_change.ToSeconds() >= procedure.start_delay_s )
			{
				ActivateProcedureSwitches( procedure, false, current_time );
				DoProcedureImmediateCommands( procedure );
				procedure_state.movement_state= ProcedureState::MovementState::Movement;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
			else
				procedure_state.movement_stage= new_stage;
			break;

		case ProcedureState::MovementState::Movement:
			if( new_stage >= 1.0f )
			{
				// TODO - do it at the end if movement?
				// Maybe, do this at end of reverse-movement?
				DoProcedureDeactivationCommands( procedure );

				procedure_state.movement_state= ProcedureState::MovementState::BackWait;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
			else
				procedure_state.movement_stage= new_stage;
			break;

		case ProcedureState::MovementState::BackWait:
		{
			const Time wait_time= current_time - procedure_state.last_state_change_time;
			if(
				procedure.back_wait_s > 0.0f &&
				wait_time.ToSeconds() >= procedure.back_wait_s )
			{
				ActivateProcedureSwitches( procedure, true, current_time );
				procedure_state.movement_state= ProcedureState::MovementState::ReverseMovement;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
		}
			break;

		case ProcedureState::MovementState::ReverseMovement:
			if( new_stage >= 1.0f )
			{
				procedure_state.movement_state= ProcedureState::MovementState::None;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
			else
				procedure_state.movement_stage= new_stage;
			break;
		}; // switch state

		// Select positions, using movement state.

		float absolute_action_stage;
		if( procedure_state.movement_state == ProcedureState::MovementState::Movement )
			absolute_action_stage= procedure_state.movement_stage;
		else if( procedure_state.movement_state == ProcedureState::MovementState::BackWait )
			absolute_action_stage= 1.0f;
		else if( procedure_state.movement_state == ProcedureState::MovementState::ReverseMovement )
			absolute_action_stage= 1.0f - procedure_state.movement_stage;
		else
			absolute_action_stage= 0.0f;

		for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
		{
			using Action= MapData::Procedure::ActionCommandId;
			switch( command.id )
			{
			case Action::Move:

			case Action::XMove: // Xmove and YMove commands looks just like move command alias.
			case Action::YMove:
			{
				const unsigned char x= static_cast<unsigned char>(command.args[0]);
				const unsigned char y= static_cast<unsigned char>(command.args[1]);
				const float dx= command.args[2] * g_commands_coords_scale;
				const float dy= command.args[3] * g_commands_coords_scale;
				const float sound_number= command.args[4];
				PC_UNUSED(sound_number);

				PC_ASSERT( x < MapData::c_map_size && y < MapData::c_map_size );
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];

				if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < map_data_->dynamic_walls.size() );

					const MapData::Wall& map_wall= map_data_->dynamic_walls[ index_element.index ];
					DynamicWall& wall= dynamic_walls_[ index_element.index ];

					for( unsigned int v= 0u; v < 2u; v++ )
					{
						wall.vert_pos[v]= map_wall.vert_pos[v];
						wall.vert_pos[v].x+= dx * absolute_action_stage;
						wall.vert_pos[v].y+= dy * absolute_action_stage;
					}
					wall.z= 0.0f;
				}
				else if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );
					const MapData::StaticModel& map_model= map_data_->static_models[ index_element.index ];
					StaticModel& model= static_models_[ index_element.index ];

					model.pos=
						m_Vec3(
							map_model.pos + m_Vec2( dx, dy ) * absolute_action_stage,
							model.baze_z );
				}
			}
				break;

			case Action::Rotate:
			{
				const unsigned char x= static_cast<unsigned char>(command.args[0]);
				const unsigned char y= static_cast<unsigned char>(command.args[1]);
				const float center_x= command.args[2] * g_commands_coords_scale;
				const float center_y= command.args[3] * g_commands_coords_scale;
				const float angle= command.args[4] * Constants::to_rad;
				const float sound_number= command.args[5];
				PC_UNUSED(sound_number);

				PC_ASSERT( x < MapData::c_map_size && y < MapData::c_map_size );

				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];

				if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < map_data_->dynamic_walls.size() );

					const MapData::Wall& map_wall= map_data_->dynamic_walls[ index_element.index ];
					DynamicWall& wall= dynamic_walls_[ index_element.index ];

					m_Mat3 rot_mat;
					rot_mat.RotateZ( angle * absolute_action_stage );
					const m_Vec2 center( center_x, center_y );

					for( unsigned int v= 0u; v < 2u; v++ )
					{
						const m_Vec2 vec= map_wall.vert_pos[v] - center;
						const m_Vec2 vec_rotated= ( m_Vec3( vec, 0.0f ) * rot_mat ).xy();
						wall.vert_pos[v]= center + vec_rotated;
					}
					wall.z= 0.0f;
				}
				else if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );
					const MapData::StaticModel& map_model= map_data_->static_models[ index_element.index ];
					StaticModel& model= static_models_[ index_element.index ];

					model.pos= m_Vec3( map_model.pos, model.baze_z );
					model.angle= map_model.angle + angle * absolute_action_stage;
				}
			}
				break;

			case Action::Up:
			{
				const unsigned char x= static_cast<unsigned char>(command.args[0]);
				const unsigned char y= static_cast<unsigned char>(command.args[1]);
				const float height= command.args[2] * g_commands_coords_scale * 4.0f;
				const float sound_number= command.args[3];
				PC_UNUSED(sound_number);

				PC_ASSERT( x < MapData::c_map_size && y < MapData::c_map_size );
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];

				if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < map_data_->dynamic_walls.size() );

					const MapData::Wall& map_wall= map_data_->dynamic_walls[ index_element.index ];
					DynamicWall& wall= dynamic_walls_[ index_element.index ];

					for( unsigned int v= 0u; v < 2u; v++ )
						wall.vert_pos[v]= map_wall.vert_pos[v];
					wall.z= height * absolute_action_stage;
				}
				else if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );
					const MapData::StaticModel& map_model= map_data_->static_models[ index_element.index ];
					StaticModel& model= static_models_[ index_element.index ];

					model.pos= m_Vec3( map_model.pos, height + model.baze_z );
				}
			}
				break;

			default:
				// TODO
				break;
			}
		} // for action commands

	} // for procedures

	// Process static models
	for( StaticModel& model : static_models_ )
	{
		const float time_delta_s= ( current_time - model.animation_start_time ).ToSeconds();
		const float animation_frame= time_delta_s * GameConstants::animations_frames_per_second;

		if( model.animation_state == StaticModel::AnimationState::Animation )
		{
			if( model.model_id < map_data_->models.size() )
			{
				const Model& model_geometry= map_data_->models[ model.model_id ];

				model.current_animation_frame=
					static_cast<unsigned int>( std::round(animation_frame) ) % model_geometry.frame_count;
			}
			else
				model.current_animation_frame= 0u;
		}
		else if( model.animation_state == StaticModel::AnimationState::SingleAnimation )
		{
			if( model.model_id < map_data_->models.size() )
			{
				const Model& model_geometry= map_data_->models[ model.model_id ];

				const unsigned int animation_frame_integer= static_cast<unsigned int>( std::round(animation_frame) );
				if( animation_frame_integer >= model_geometry.frame_count - 1u )
				{
					model.animation_state= StaticModel::AnimationState::SingleFrame;
					model.animation_start_frame= model_geometry.frame_count - 1u;
				}
				else
					model.current_animation_frame= animation_frame_integer;
			}
			else
				model.current_animation_frame= 0u;
		}
		else if( model.animation_state == StaticModel::AnimationState::SingleReverseAnimation )
		{
			if( model.model_id < map_data_->models.size() )
			{
				const int animation_frame_integer=
					int(model.animation_start_frame) - static_cast<int>( std::round(animation_frame) );
				if( animation_frame_integer <= 0 )
				{
					model.animation_state= StaticModel::AnimationState::SingleFrame;
					model.animation_start_frame= 0u;
				}
				else
					model.current_animation_frame= animation_frame_integer;
			}
			else
				model.current_animation_frame= 0u;
		}
		else if( model.animation_state == StaticModel::AnimationState::SingleFrame )
			model.current_animation_frame= model.animation_start_frame;
		else
			model.current_animation_frame= model.animation_start_frame;
	} // for static models

	// Process shots
	for( unsigned int r= 0u; r < rockets_.size(); )
	{
		Rocket& rocket= rockets_[r];
		const GameResources::RocketDescription& rocket_description= game_resources_->rockets_description[ rocket.rocket_type_id ];

		const bool has_infinite_speed= rocket.HasInfiniteSpeed( *game_resources_ );
		const float time_delta_s= ( current_time - rocket.start_time ).ToSeconds();

		HitResult hit_result;

		if( has_infinite_speed )
			hit_result= ProcessShot( rocket.start_point, rocket.normalized_direction, rocket.owner_id );
		else
		{
			// TODO - process rockets with nontrivial trajectories - reflecting, autoaim.
			const float gravity_force= GameConstants::rockets_gravity_scale * float( rocket_description.gravity_force );
			const float speed= rocket_description.fast ? GameConstants::fast_rockets_speed : GameConstants::rockets_speed;

			const m_Vec3 new_pos=
				rocket.start_point +
				rocket.normalized_direction * ( time_delta_s * speed ) +
				m_Vec3( 0.0f, 0.0f, -1.0f ) * ( gravity_force * time_delta_s * time_delta_s * 0.5f );

			m_Vec3 dir= new_pos - rocket.previous_position;
			dir.Normalize();

			hit_result= ProcessShot( rocket.previous_position, dir, rocket.owner_id );

			if( hit_result.object_type != HitResult::ObjectType::None )
			{
				const float hit_pos_vecs_dot=
					( new_pos - hit_result.pos ) * ( rocket.previous_position - hit_result.pos );

				if( hit_pos_vecs_dot > 0.0f )
					hit_result.object_type= HitResult::ObjectType::None; // Really, not hited
			}

			// Emit smoke trail
			const unsigned int sprite_effect_id=
				game_resources_->rockets_description[ rocket.rocket_type_id ].smoke_trail_effect_id;
			if( sprite_effect_id != 0u )
			{
				const float c_particels_per_unit= 2.0f; // TODO - calibrate
				const float length_delta= ( new_pos - rocket.previous_position ).Length() * c_particels_per_unit;
				const float new_track_length= rocket.track_length + length_delta;
				for( unsigned int i= static_cast<unsigned int>( rocket.track_length ) + 1u;
					i <= static_cast<unsigned int>( new_track_length ); i++ )
				{
					const float part= ( float(i) - rocket.track_length ) / length_delta;

					sprite_effects_.emplace_back();
					SpriteEffect& effect= sprite_effects_.back();

					effect.pos= ( 1.0f - part ) * rocket.previous_position + part * new_pos;
					effect.effect_id= sprite_effect_id;
				}

				rocket.track_length= new_track_length;
			}

			rocket.previous_position= new_pos;
		}

		// Gen hit effect
		const float c_walls_effect_offset= 1.0f / 32.0f;
		if( hit_result.object_type == HitResult::ObjectType::StaticWall )
		{
			GenParticleEffectForRocketHit(
				hit_result.pos + GetNormalForWall( map_data_->static_walls[ hit_result.object_index ] ) * c_walls_effect_offset,
				rocket.rocket_type_id );
		}
		else if( hit_result.object_type == HitResult::ObjectType::DynamicWall )
		{
			GenParticleEffectForRocketHit(
				hit_result.pos + GetNormalForWall( dynamic_walls_[ hit_result.object_index ] ) * c_walls_effect_offset,
				rocket.rocket_type_id );
		}
		else if( hit_result.object_type == HitResult::ObjectType::Floor )
		{
			GenParticleEffectForRocketHit(
				hit_result.pos + m_Vec3( 0.0f, 0.0f, ( hit_result.object_index == 0 ? 1.0f : -1.0f ) * c_walls_effect_offset ),
				rocket.rocket_type_id );
		}
		else if( hit_result.object_type == HitResult::ObjectType::Model )
			GenParticleEffectForRocketHit( hit_result.pos, rocket.rocket_type_id );
		else if( hit_result.object_type == HitResult::ObjectType::Monster )
		{
			AddParticleEffect( hit_result.pos, ParticleEffect::Blood );

			// Hack for rockets and grenades. Make effect together with blood.
			if( rocket_description.blow_effect == 2 && !has_infinite_speed )
				GenParticleEffectForRocketHit( hit_result.pos, rocket.rocket_type_id );
		}

		// Try break breakable models.
		if( hit_result.object_type == HitResult::ObjectType::Model )
		{
			StaticModel& model= static_models_[ hit_result.object_index ];

			if( model.model_id >= map_data_->models_description.size() )
				goto end_loop;

			const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];
			if( model_description.break_limit <= 0 )
			{
				// Not breakable - process shot.
				ProcessElementLinks(
					MapData::IndexElement::StaticModel,
					hit_result.object_index,
					[&]( const MapData::Link& link )
					{
						if( link.type == MapData::Link::Shoot )
							ProcedureProcessShoot( link.proc_id, current_time );
					} );
			}
			else
			{
				model.health-= int(rocket_description.power);
				if( model.health <= 0 )
				{
					EmitModelDestructionEffects( hit_result.object_index );

					model.model_id++; // now, this model has other model type
					if( model.model_id < map_data_->models_description.size() )
						model.health= map_data_->models_description[ model.model_id ].break_limit;
					else
						model.health= 0;

					ProcessElementLinks(
						MapData::IndexElement::StaticModel,
						hit_result.object_index,
						[&]( const MapData::Link& link )
						{
							if( link.type == MapData::Link::Destroy )
								ProcedureProcessDestroy( link.proc_id, current_time );
						} );
				}
			}
		}
		else if(
			hit_result.object_type == HitResult::ObjectType::StaticWall ||
			hit_result.object_type == HitResult::ObjectType::DynamicWall )
		{
			ProcessElementLinks(
				hit_result.object_type == HitResult::ObjectType::StaticWall
					? MapData::IndexElement::StaticWall
					: MapData::IndexElement::DynamicWall,
				hit_result.object_index,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Shoot )
						ProcedureProcessShoot( link.proc_id, current_time );
				} );
		}
		else if( hit_result.object_type == HitResult::ObjectType::Floor )
		{
			// TODO - support rockets reflections
		}
		else if( hit_result.object_type == HitResult::ObjectType::Monster )
		{
			auto it= monsters_.find( hit_result.object_index );
			PC_ASSERT( it != monsters_.end() );

			const MonsterBasePtr& monster= it->second;
			PC_ASSERT( monster != nullptr );
			monster->Hit( rocket_description.power, current_time );
		}

	end_loop:
		// Try remove rocket
		if( hit_result.object_type != HitResult::ObjectType::None || // kill hited
			time_delta_s > 16.0f || // kill old rockets
			has_infinite_speed ) // kill bullets
		{
			if( !has_infinite_speed )
			{
				rockets_death_messages_.emplace_back();
				rockets_death_messages_.back().rocket_id= rocket.rocket_id;
			}

			if( r != rockets_.size() - 1u )
				rockets_[r]= rockets_.back();
			rockets_.pop_back();
		}
		else
			r++;
	} // for rockets

	// Process monsters
	for( MonstersContainer::value_type& monster_value : monsters_ )
	{
		monster_value.second->Tick( *this, monster_value.first, current_time, last_tick_delta );

		// Process teleports for monster
		MonsterBase& monster= *monster_value.second;
		const bool is_player= monster.MonsterId() == 0u;;

		const float monster_radius= is_player ? GameConstants::player_radius : game_resources_->monsters_description[ monster.MonsterId() ].w_radius;
		const float teleport_radius= monster_radius + 0.1f;

		for( const MapData::Teleport& teleport : map_data_->teleports )
		{
			const m_Vec2 tele_pos( float(teleport.from[0]) + 0.5f, float(teleport.from[1]) + 0.5f );

			if( ( tele_pos - monster.Position().xy() ).SquareLength() >= teleport_radius * teleport_radius )
				continue;

			m_Vec2 dst;
			for( unsigned int j= 0u; j < 2u; j++ )
			{
				if( teleport.to[j] >= MapData::c_map_size )
					dst.ToArr()[j]= float( teleport.to[j] ) / 256.0f;
				else
					dst.ToArr()[j]= float( teleport.to[j] );
			}
			monster.Teleport(
				m_Vec3(
					dst,
					GetFloorLevel( dst, GameConstants::player_radius ) ),
				teleport.angle );
			break;
		}

		// Process wind for monster
		// TODO - select more correct way to do this.
		// Calculate intersection between monster circle and wind field cells.
		const int monster_x= static_cast<int>( monster.Position().x );
		const int monster_y= static_cast<int>( monster.Position().y );
		if( monster_x >= 0 && monster_x < int(MapData::c_map_size) &&
			monster_y >= 0 && monster_y < int(MapData::c_map_size) )
		{
			const char* const wind_cell= wind_field_[ monster_x + monster_y * int(MapData::c_map_size) ];
			if( wind_cell[0] == 0 && wind_cell[1] == 0 )
				continue;

			const float time_delta_s= last_tick_delta.ToSeconds();
			const float c_wind_power_scale= 0.5f;
			const m_Vec2 pos_delta= time_delta_s * c_wind_power_scale * m_Vec2( float(wind_cell[0]), float(wind_cell[1]) );

			monster.SetPosition( monster.Position() + m_Vec3( pos_delta, 0.0f ) );
		}
	}

	// Collide monsters with map
	for( MonstersContainer::value_type& monster_value : monsters_ )
	{
		MonsterBase& monster= *monster_value.second;
		const bool is_player= monster.MonsterId() == 0u;

		if( is_player && static_cast<const Player&>(monster).IsNoclip() )
			continue;

		const float height= GameConstants::player_height; // TODO - select height
		const float radius= is_player ? GameConstants::player_radius : game_resources_->monsters_description[ monster.MonsterId() ].w_radius;

		bool on_floor= false;
		const m_Vec3 old_monster_pos= monster.Position();
		const m_Vec3 new_monster_pos=
			CollideWithMap(
				old_monster_pos, height, radius,
				on_floor );

		const m_Vec3 position_delta= new_monster_pos - old_monster_pos;

		if( position_delta.z != 0.0f ) // Vertical clamp
			monster.ClampSpeed( m_Vec3( 0.0f, 0.0f, position_delta.z > 0.0f ? 1.0f : -1.0f ) );

		const float position_delta_length= position_delta.xy().Length();
		if( position_delta_length != 0.0f ) // Horizontal clamp
			monster.ClampSpeed( m_Vec3( position_delta.xy() / position_delta_length, 0.0f ) );

		monster.SetPosition( new_monster_pos );
		monster.SetOnFloor( on_floor );
	}

	// Collide monsters together
	for( MonstersContainer::value_type& first_monster_value : monsters_ )
	{
		MonsterBase& first_monster= *first_monster_value.second;
		if( first_monster.Health() <= 0 )
			continue;

		const float first_monster_radius= game_resources_->monsters_description[ first_monster.MonsterId() ].w_radius;
		const m_Vec2 first_monster_z_minmax=
			first_monster.GetZMinMax() + m_Vec2( first_monster.Position().z, first_monster.Position().z );

		for( MonstersContainer::value_type& second_monster_value : monsters_ )
		{
			MonsterBase& second_monster= *second_monster_value.second;
			if( &second_monster == &first_monster )
				continue;

			if( second_monster.Health() <= 0 )
				continue;

			const float square_distance= ( first_monster.Position().xy() - second_monster.Position().xy() ).SquareLength();

			const float c_max_collide_distance= 8.0f;
			if( square_distance > c_max_collide_distance * c_max_collide_distance )
				continue;

			const float second_monster_radius= game_resources_->monsters_description[ second_monster.MonsterId() ].w_radius;
			const float min_distance= second_monster_radius + first_monster_radius;
			if( square_distance > min_distance * min_distance )
				continue;

			const m_Vec2 second_monster_z_minmax=
				second_monster.GetZMinMax() + m_Vec2( second_monster.Position().z, second_monster.Position().z );
			if(  first_monster_z_minmax.y < second_monster_z_minmax.x ||
				second_monster_z_minmax.y <  first_monster_z_minmax.x ) // Z check
				continue;

			// Collide here
			m_Vec2 collide_vec= second_monster.Position().xy() - first_monster.Position().xy();
			collide_vec.Normalize();

			const float move_delta= min_distance - std::sqrt( square_distance );

			float first_monster_k;
			if( first_monster.MonsterId() == 0u && second_monster.MonsterId() != 0u )
				first_monster_k= 1.0f;
			else if( first_monster.MonsterId() != 0u && second_monster.MonsterId() == 0u )
				first_monster_k= 0.0f;
			else
				first_monster_k= 0.5f;

			const m_Vec2  first_monster_pos=  first_monster.Position().xy() - collide_vec * move_delta * first_monster_k;
			const m_Vec2 second_monster_pos= second_monster.Position().xy() + collide_vec * move_delta * ( 1.0f - first_monster_k );

			 first_monster.SetPosition( m_Vec3( first_monster_pos ,  first_monster.Position().z ) );
			second_monster.SetPosition( m_Vec3( second_monster_pos, second_monster.Position().z ) );
		}
	}

	// At end of this procedure, report about map change, if this needed.
	// Do it here, because map can be desctructed at callback call.
	if( map_end_triggered_ &&
		map_end_callback_ != nullptr )
	{
		map_end_triggered_= false;
		map_end_callback_();
	}
}

void Map::SendMessagesForNewlyConnectedPlayer( MessagesSender& messages_sender ) const
{
	// Send monsters
	for( const MonstersContainer::value_type& monster_entry : monsters_ )
	{
		Messages::MonsterBirth message;

		PrepareMonsterStateMessage( *monster_entry.second, message.initial_state );
		message.initial_state.monster_id= monster_entry.first;
		message.monster_id= monster_entry.first;

		messages_sender.SendReliableMessage( message );
	}
}

void Map::SendUpdateMessages( MessagesSender& messages_sender ) const
{
	Messages::WallPosition wall_message;

	for( const DynamicWall& wall : dynamic_walls_ )
	{
		wall_message.wall_index= &wall - dynamic_walls_.data();

		PositionToMessagePosition( wall.vert_pos[0], wall_message.vertices_xy[0] );
		PositionToMessagePosition( wall.vert_pos[1], wall_message.vertices_xy[1] );
		wall_message.z= CoordToMessageCoord( wall.z );
		wall_message.texture_id= wall.texture_id;

		messages_sender.SendUnreliableMessage( wall_message );
	}

	Messages::StaticModelState model_message;

	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const StaticModel& model= static_models_[m];

		model_message.static_model_index= m;
		model_message.animation_frame= model.current_animation_frame;
		model_message.animation_playing= model.animation_state == StaticModel::AnimationState::Animation;
		model_message.model_id= model.model_id;

		PositionToMessagePosition( model.pos, model_message.xyz );
		model_message.angle= AngleToMessageAngle( model.angle );

		messages_sender.SendUnreliableMessage( model_message );
	}

	for( const Item& item : items_ )
	{
		Messages::ItemState message;
		message.item_index= &item - items_.data();
		message.z= CoordToMessageCoord( item.pos.z );
		message.picked= item.picked_up;

		messages_sender.SendUnreliableMessage( message );
	}

	Messages::SpriteEffectBirth sprite_message;

	for( const SpriteEffect& effect : sprite_effects_ )
	{
		sprite_message.effect_id= effect.effect_id;
		PositionToMessagePosition( effect.pos, sprite_message.xyz );

		messages_sender.SendUnreliableMessage( sprite_message );
	}

	for( const MonstersContainer::value_type& monster_value : monsters_ )
	{
		Messages::MonsterState monster_message;

		PrepareMonsterStateMessage( *monster_value.second, monster_message );
		monster_message.monster_id= monster_value.first;

		messages_sender.SendUnreliableMessage( monster_message );
	}

	for( const Messages::RocketBirth& message : rockets_birth_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::RocketDeath& message : rockets_death_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::ParticleEffectBirth& message : particles_effects_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}

	for( const Rocket& rocket : rockets_ )
	{
		Messages::RocketState rocket_message;

		rocket_message.rocket_id= rocket.rocket_id;
		PositionToMessagePosition( rocket.previous_position, rocket_message.xyz );

		float angle[2];
		VecToAngles( rocket.normalized_direction, angle );
		for( unsigned int j= 0u; j < 2u; j++ )
			rocket_message.angle[j]= AngleToMessageAngle( angle[j] );

		messages_sender.SendUnreliableMessage( rocket_message );
	}
}

void Map::ClearUpdateEvents()
{
	sprite_effects_.clear();
	rockets_birth_messages_.clear();
	rockets_death_messages_.clear();
	particles_effects_messages_.clear();
}

void Map::ActivateProcedure( const unsigned int procedure_number, const Time current_time )
{
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	procedure_state.movement_stage= 0.0f;
	procedure_state.movement_state= ProcedureState::MovementState::StartWait;
	procedure_state.last_state_change_time= current_time;
}

void Map::TryActivateProcedure(
	unsigned int procedure_number,
	const Time current_time,
	Player& player,
	MessagesSender& messages_sender )
{
	if( procedure_number == 0u )
		return;

	if( !player.TryActivateProcedure( procedure_number, current_time ) )
		return;

	PC_ASSERT( procedure_number < procedures_.size() );

	const MapData::Procedure& procedure= map_data_->procedures[ procedure_number ];
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	const bool have_necessary_keys=
		( !procedure.  red_key_required || player.HaveRedKey() ) &&
		( !procedure.green_key_required || player.HaveGreenKey() ) &&
		( !procedure. blue_key_required || player.HaveBlueKey() );

	if(
		have_necessary_keys &&
		!procedure_state.locked &&
		procedure_state.movement_state == ProcedureState::MovementState::None )
	{
		ActivateProcedure( procedure_number, current_time );
	} // if activated

	// Activation messages.
	if( procedure.first_message_number != 0u &&
		!procedure_state.first_message_printed )
	{
		procedure_state.first_message_printed= true;

		Messages::TextMessage text_message;
		text_message.text_message_number= procedure.first_message_number;
		messages_sender.SendUnreliableMessage( text_message );
	}
	if( procedure.lock_message_number != 0u &&
		( procedure_state.locked || !have_necessary_keys ) )
	{
		Messages::TextMessage text_message;
		text_message.text_message_number= procedure.lock_message_number;
		messages_sender.SendUnreliableMessage( text_message );
	}
	if( procedure.on_message_number != 0u )
	{
		Messages::TextMessage text_message;
		text_message.text_message_number= procedure.on_message_number;
		messages_sender.SendUnreliableMessage( text_message );
	}
}

void Map::ProcedureProcessDestroy( const unsigned int procedure_number, const Time current_time )
{
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	// Autol-unlock locked procedures
	procedure_state.locked= false;

	ActivateProcedure( procedure_number, current_time );
}

void Map::ProcedureProcessShoot( const unsigned int procedure_number, const Time current_time )
{
	ActivateProcedure( procedure_number, current_time );
}

void Map::ActivateProcedureSwitches( const MapData::Procedure& procedure, const bool inverse_animation, const Time current_time )
{
	for( const MapData::Procedure::SwitchPos& siwtch_pos : procedure.linked_switches )
	{
		if( siwtch_pos.x >= MapData::c_map_size || siwtch_pos.y >= MapData::c_map_size )
			continue;

		const MapData::IndexElement& index_element= map_data_->map_index[ siwtch_pos.x + siwtch_pos.y * MapData::c_map_size ];
		if( index_element.type == MapData::IndexElement::StaticModel )
		{
			PC_ASSERT( index_element.index < static_models_.size() );
			StaticModel& model= static_models_[ index_element.index ];

			if( model.animation_state == StaticModel::AnimationState::SingleFrame )
			{
				model.animation_start_time= current_time;

				if( inverse_animation )
				{
					model.animation_state= StaticModel::AnimationState::SingleReverseAnimation;
					if( model.model_id < map_data_->models.size() )
						model.animation_start_frame= map_data_->models[ model.model_id ].frame_count - 1u;
					else
						model.animation_start_frame= 0u;
				}
				else
				{
					model.animation_state= StaticModel::AnimationState::SingleAnimation;
					model.animation_start_frame= 0u;
				}
			}
		}
	}
}

void Map::DoProcedureImmediateCommands( const MapData::Procedure& procedure )
{
	// Do immediate commands
	for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
	{
		using Command= MapData::Procedure::ActionCommandId;
		if( command.id == Command::Lock )
		{
			const unsigned short proc_number= static_cast<unsigned short>( command.args[0] );
			PC_ASSERT( proc_number < procedures_.size() );

			procedures_[ proc_number ].locked= true;
		}
		else if( command.id == Command::Unlock )
		{
			const unsigned short proc_number= static_cast<unsigned short>( command.args[0] );
			PC_ASSERT( proc_number < procedures_.size() );

			procedures_[ proc_number ].locked= false;
		}
		// TODO - know, how animation commands works
		else if( command.id == Command::PlayAnimation )
		{}
		else if( command.id == Command::StopAnimation )
		{}
		else if( command.id == Command::Change )
		{
			const unsigned int x= static_cast<unsigned int>( command.args[0] );
			const unsigned int y= static_cast<unsigned int>( command.args[1] );
			const unsigned int id= static_cast<unsigned int>( command.args[2] );
			if( x < MapData::c_map_size && y < MapData::c_map_size )
			{
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];
				if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );

					StaticModel& model = static_models_[ index_element.index ];

					// Reset Animation, if model changed.
					if( model.model_id < map_data_->models_description.size())
					{
						if( ACode( map_data_->models_description[ model.model_id ].ac ) == ACode::Switch)
						{
							model.animation_start_frame= 0u;
							model.animation_state= StaticModel::AnimationState::SingleFrame;
						}
					}
					else
					{
						model.animation_start_frame= 0;
						model.animation_state= StaticModel::AnimationState::Animation;
					}

					model.model_id= id - 163u;
				}
				else if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < dynamic_walls_.size() );
					dynamic_walls_[ index_element.index ].texture_id= id;
				}
			}
		}
		else if( command.id == Command::Wind )
		{
			ProcessWind( command, true );
		}
		// TODO - process other commands
		else
		{}
	}
}

void Map::DoProcedureDeactivationCommands( const MapData::Procedure& procedure )
{
	using Command= MapData::Procedure::ActionCommandId;

	// Check nonstop.
	// TODO - set nonstop as procedure flag, not as action command.
	for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
		if( command.id == Command::Nonstop )
			return;

	for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
	{
		if( command.id == Command::Wind )
			ProcessWind( command, false );
	}
}

void Map::ProcessWind( const MapData::Procedure::ActionCommand& command, bool activate )
{
	PC_ASSERT( command.id == MapData::Procedure::ActionCommandId::Wind );

	const unsigned int x0= static_cast<unsigned int>( command.args[0] );
	const unsigned int y0= static_cast<unsigned int>( command.args[1] );
	const unsigned int x1= static_cast<unsigned int>( command.args[2] );
	const unsigned int y1= static_cast<unsigned int>( command.args[3] );
	const int dir_x= static_cast<int>( command.args[4] );
	const int dir_y= static_cast<int>( command.args[5] );

	for( unsigned int y= y0; y <= y1 && y < MapData::c_map_size; y++ )
	for( unsigned int x= x0; x <= x1 && x < MapData::c_map_size; x++ )
	{
		char* const cell= wind_field_[ x + y * MapData::c_map_size ];
		if( activate )
		{
			cell[0]= dir_x;
			cell[1]= dir_y;
		}
		else
			cell[0]= cell[1]= 0;
	}
}

Map::HitResult Map::ProcessShot(
	const m_Vec3& shot_start_point,
	const m_Vec3& shot_direction_normalized,
	const Messages::EntityId skip_monster_id ) const
{
	HitResult result;
	float nearest_shot_point_square_distance= Constants::max_float;

	const auto process_candidate_shot_pos=
	[&]( const m_Vec3& candidate_pos, const HitResult::ObjectType object_type, const unsigned int object_index )
	{
		const float square_distance= ( candidate_pos - shot_start_point ).SquareLength();
		if( square_distance < nearest_shot_point_square_distance )
		{
			result.pos= candidate_pos;
			nearest_shot_point_square_distance= square_distance;

			result.object_type= object_type;
			result.object_index= object_index;
		}
	};

	// Static walls
	for( const MapData::Wall& wall : map_data_->static_walls )
	{
		const MapData::WallTextureDescription& wall_texture= map_data_->walls_textures[ wall.texture_id ];
		if( wall_texture.gso[1] )
			continue;

		m_Vec3 candidate_pos;
		if( RayIntersectWall(
				wall.vert_pos[0], wall.vert_pos[1],
				0.0f, 2.0f,
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::StaticWall, &wall - map_data_->static_walls.data() );
		}
	}

	// Dynamic walls
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		const MapData::WallTextureDescription& wall_texture=
			map_data_->walls_textures[ map_data_->dynamic_walls[w].texture_id ];
		if( wall_texture.gso[1] )
			continue;

		const DynamicWall& wall= dynamic_walls_[w];

		m_Vec3 candidate_pos;
		if( RayIntersectWall(
				wall.vert_pos[0], wall.vert_pos[1],
				wall.z, wall.z + 2.0f,
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::DynamicWall, w );
		}
	}

	// Models
	for( const StaticModel& model : static_models_ )
	{
		if( model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];
		if( model_description.radius <= 0.0f )
			continue;

		const Model& model_data= map_data_->models[ model.model_id ];

		m_Vec3 candidate_pos;
		if( RayIntersectCylinder(
				model.pos.xy(), model_description.radius,
				model_data.z_min + model.pos.z,
				model_data.z_max + model.pos.z,
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::Model, &model - static_models_.data() );
		}
	}

	// Monsters
	for( const MonstersContainer::value_type& monster_value : monsters_ )
	{
		if( monster_value.first == skip_monster_id )
			continue;

		m_Vec3 candidate_pos;
		if( monster_value.second->TryShot(
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			process_candidate_shot_pos(
				candidate_pos, HitResult::ObjectType::Monster,
				monster_value.first );
		}
	}

	// Floors, ceilings
	for( unsigned int z= 0u; z <= 2u; z+= 2u )
	{
		m_Vec3 candidate_pos;
		if( RayIntersectXYPlane(
				float(z),
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			const int x= static_cast<int>( std::floor(candidate_pos.x) );
			const int y= static_cast<int>( std::floor(candidate_pos.y) );
			if( x < 0 || x >= int(MapData::c_map_size) ||
				y < 0 || y >= int(MapData::c_map_size) )
				continue;

			const int coord= x + y * int(MapData::c_map_size);
			const unsigned char texture_id=
				( z == 0 ? map_data_->floor_textures : map_data_->ceiling_textures )[ coord ];

			if( texture_id == MapData::c_empty_floor_texture_id ||
				texture_id == MapData::c_sky_floor_texture_id )
				continue;

			process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::Floor, z >> 1u );
		}
	}

	return result;
}

float Map::GetFloorLevel( const m_Vec2& pos, const float radius ) const
{
	float max_dz= 0.0f;

	const float c_max_floor_level= 1.2f;

	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const MapData::StaticModel& map_model= map_data_->static_models[m];
		if( map_model.is_dynamic )
			continue;

		if( map_model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ map_model.model_id ];
		if( model_description.ac != 0u )
			continue;

		const float model_radius= model_description.radius;
		if( model_radius <= 0.0f )
			continue;

		const float square_distance= ( pos - map_model.pos ).SquareLength();
		const float collision_distance= model_radius + radius;
		if( square_distance > collision_distance * collision_distance )
			continue;

		// Hit here
		const Model& model= map_data_->models[ map_model.model_id ];

		if( model.z_max >= c_max_floor_level )
			continue;

		max_dz= std::max( max_dz, model.z_max );
	}

	return max_dz;
}

Messages::EntityId Map::GetNextMonsterId()
{
	return ++next_monster_id_;
}

void Map::PrepareMonsterStateMessage( const MonsterBase& monster, Messages::MonsterState& message )
{
	PositionToMessagePosition( monster.Position(), message.xyz );
	message.angle= AngleToMessageAngle( monster.Angle() );
	message.monster_type= monster.MonsterId();
	message.animation= monster.CurrentAnimation();
	message.animation_frame= monster.CurrentAnimationFrame();
}

void Map::EmitModelDestructionEffects( const unsigned int model_number )
{
	PC_ASSERT( model_number < static_models_.size() );
	const StaticModel& model= static_models_[ model_number ];

	if( model.model_id >= map_data_->models_description.size() )
		return;

	const MapData::ModelDescription& description= map_data_->models_description[ model.model_id ];
	const Model& model_data= map_data_->models[ model.model_id ];

	const unsigned int blow_effect_id= description.blw % 100u;

	m_Vec3 pos= model.pos;
	// TODO - tune this formula. It can be invalid.
	pos.z+= ( model_data.z_min + model_data.z_max ) * 0.5f + float( description.bmpz ) / 128.0f;

	particles_effects_messages_.emplace_back();
	Messages::ParticleEffectBirth& message= particles_effects_messages_.back();

	PositionToMessagePosition( pos, message.xyz );
	message.effect_id= static_cast<unsigned char>( ParticleEffect::FirstBlowEffect ) + blow_effect_id;
}

void Map::AddParticleEffect( const m_Vec3& pos, const ParticleEffect particle_effect )
{
	particles_effects_messages_.emplace_back();
	Messages::ParticleEffectBirth& message= particles_effects_messages_.back();

	PositionToMessagePosition( pos, message.xyz );
	message.effect_id= static_cast<unsigned char>( particle_effect );
}

void Map::GenParticleEffectForRocketHit( const m_Vec3& pos, const unsigned int rocket_type_id )
{
	PC_ASSERT( rocket_type_id < game_resources_->rockets_description.size() );
	const GameResources::RocketDescription& description= game_resources_->rockets_description[ rocket_type_id ];

	Messages::ParticleEffectBirth* message= nullptr;

	if( description.model_file_name[0] == '\0' )
	{ // bullet
		if( description.blow_effect == 1 )
		{
			//bullet
			particles_effects_messages_.emplace_back();
			message= & particles_effects_messages_.back();
			message->effect_id= static_cast<unsigned char>( ParticleEffect::Bullet );
		}
	}
	else
	{
		if( description.blow_effect == 1 || description.blow_effect == 3 || description.blow_effect == 4 )
		{
			// sparcles
			particles_effects_messages_.emplace_back();
			message= & particles_effects_messages_.back();
			message->effect_id= static_cast<unsigned char>( ParticleEffect::Sparkles );
		}
		if( description.blow_effect == 2 )
		{
			//explosion
			particles_effects_messages_.emplace_back();
			message= & particles_effects_messages_.back();
			message->effect_id= static_cast<unsigned char>( ParticleEffect::Explosion );
		}
		if( description.blow_effect == 4 )
		{
			// Mega destroyer flash - TODO
		}
	}

	if( message != nullptr )
		PositionToMessagePosition( pos, message->xyz );
}

} // PanzerChasm
