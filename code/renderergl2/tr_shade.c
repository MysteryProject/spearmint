/*
===========================================================================
Copyright (C) 1999-2010 id Software LLC, a ZeniMax Media company.

This file is part of Spearmint Source Code.

Spearmint Source Code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 3 of the License,
or (at your option) any later version.

Spearmint Source Code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Spearmint Source Code.  If not, see <http://www.gnu.org/licenses/>.

In addition, Spearmint Source Code is also subject to certain additional terms.
You should have received a copy of these additional terms immediately following
the terms and conditions of the GNU General Public License.  If not, please
request a copy in writing from id Software at the address below.

If you have questions concerning this license or the applicable additional
terms, you may contact in writing id Software LLC, c/o ZeniMax Media Inc.,
Suite 120, Rockville, Maryland 20850 USA.
===========================================================================
*/
// tr_shade.c

#include "tr_local.h" 

/*

  THIS ENTIRE FILE IS BACK END

  This file deals with applying shaders to surface data in the tess struct.
*/


/*
==================
R_DrawElements

==================
*/

void R_DrawElements( int numIndexes, int firstIndex )
{
	if (tess.useCacheVao)
	{
		VaoCache_DrawElements(numIndexes, firstIndex);
	}
	else
	{
		qglDrawElements(GL_TRIANGLES, numIndexes, GL_INDEX_TYPE, BUFFER_OFFSET(firstIndex * sizeof(glIndex_t)));
	}
}


/*
=============================================================

SURFACE SHADERS

=============================================================
*/

shaderCommands_t	tess;


/*
=================
R_BindAnimatedImageToTMU

=================
*/
static void R_BindAnimatedImageToTMU( textureBundle_t *bundle, int tmu ) {
	int64_t index;

	if ( bundle->isVideoMap ) {
		ri.CIN_RunCinematic(bundle->videoMapHandle);
		ri.CIN_UploadCinematic(bundle->videoMapHandle);
		GL_BindToTMU(tr.scratchImage[bundle->videoMapHandle], tmu);
		return;
	}

	if ( bundle->numImageAnimations <= 1 ) {
		GL_BindToTMU( bundle->image[0], tmu);
		return;
	}

	// it is necessary to do this messy calc to make sure animations line up
	// exactly with waveforms of the same frequency
	index = tess.shaderTime * bundle->imageAnimationSpeed * FUNCTABLE_SIZE;
	index >>= FUNCTABLE_SIZE2;

	if ( index < 0 ) {
		index = 0;	// may happen with shader time offsets
	}

	if ( bundle->loopingImageAnim ) {
		// Windows x86 doesn't load renderer DLL with 64 bit modulus
		//index %= bundle->numImageAnimations;
		while ( index >= bundle->numImageAnimations ) {
			index -= bundle->numImageAnimations;
		}
	} else if ( index >= bundle->numImageAnimations ) {
		index = bundle->numImageAnimations-1;
	}

	GL_BindToTMU( bundle->image[ index ], tmu );
}


/*
================
DrawTris

Draws triangle outlines for debugging
================
*/
static void DrawTris (shaderCommands_t *input) {
	GL_BindToTMU( tr.whiteImage, TB_COLORMAP );

	GL_State( GLS_POLYMODE_LINE | GLS_DEPTHMASK_TRUE );
	qglDepthRange( 0, 0 );

	{
		shaderProgram_t *sp = &tr.textureColorShader;
		vec4_t color;

		GLSL_BindProgram(sp);
		
		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		VectorSet4(color, 1, 1, 1, 1);
		GLSL_SetUniformVec4(sp, UNIFORM_COLOR, color);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		R_DrawElements(input->numIndexes, input->firstIndex);
	}

	qglDepthRange( 0, 1 );
}


/*
================
DrawNormals

Draws vertex normals for debugging
================
*/
static void DrawNormals (shaderCommands_t *input) {
	//FIXME: implement this
}

/*
==============
RB_BeginSurface

We must set some things up before beginning any tesselation,
because a surface may be forced to perform a RB_End due
to overflow.
==============
*/
void RB_BeginSurface( shader_t *shader, int fogNum, int cubemapIndex ) {

	shader_t *state = (shader->remappedShader) ? shader->remappedShader : shader;

	tess.numIndexes = 0;
	tess.firstIndex = 0;
	tess.numVertexes = 0;
	tess.shader = state;
	tess.fogNum = fogNum;
	tess.cubemapIndex = cubemapIndex;
	tess.dlightBits = 0;		// will be OR'd in by surface functions
	tess.pshadowBits = 0;       // will be OR'd in by surface functions
	tess.xstages = state->stages;
	tess.numPasses = state->numUnfoggedPasses;
	tess.currentStageIteratorFunc = state->optimalStageIteratorFunc;
	tess.useInternalVao = qtrue;
	tess.useCacheVao = qfalse;

	tess.shaderTime = backEnd.refdef.floatTime - tess.shader->timeOffset;
	if (tess.shader->clampTime && tess.shaderTime >= tess.shader->clampTime) {
		tess.shaderTime = tess.shader->clampTime;
	}

	if (backEnd.viewParms.flags & VPF_SHADOWMAP)
	{
		tess.currentStageIteratorFunc = RB_StageIteratorGeneric;
	}
}



extern float EvalWaveForm( const waveForm_t *wf );
extern float EvalWaveFormClamped( const waveForm_t *wf );


static void ComputeTexMods( shaderStage_t *pStage, int bundleNum, vec4_t outMatrix[8])
{
	int tm;
	float matrix[6];
	float tmpmatrix[6];
	float currentmatrix[6];
	float turb[2];
	textureBundle_t *bundle = &pStage->bundle[bundleNum];
	qboolean hasTurb = qfalse;

	currentmatrix[0] = 1.0f; currentmatrix[2] = 0.0f; currentmatrix[4] = 0.0f;
	currentmatrix[1] = 0.0f; currentmatrix[3] = 1.0f; currentmatrix[5] = 0.0f;

	for ( tm = 0; tm < bundle->numTexMods ; tm++ ) {
		switch ( bundle->texMods[tm].type )
		{
			
		case TMOD_NONE:
			matrix[0] = 1.0f; matrix[2] = 0.0f; matrix[4] = 0.0f;
			matrix[1] = 0.0f; matrix[3] = 1.0f; matrix[5] = 0.0f;
			break;

		case TMOD_TURBULENT:
			RB_CalcTurbulentFactors(&bundle->texMods[tm].wave, &turb[0], &turb[1]);
			break;

		case TMOD_ENTITY_TRANSLATE:
			RB_CalcScrollTexMatrix( backEnd.currentEntity->e.shaderTexCoord, matrix );
			break;

		case TMOD_SCROLL:
			RB_CalcScrollTexMatrix( bundle->texMods[tm].scroll,
									 matrix );
			break;

		case TMOD_SCALE:
			RB_CalcScaleTexMatrix( bundle->texMods[tm].scale,
								  matrix );
			break;
		
		case TMOD_STRETCH:
			RB_CalcStretchTexMatrix( &bundle->texMods[tm].wave, 
								   matrix );
			break;

		case TMOD_TRANSFORM:
			RB_CalcTransformTexMatrix( &bundle->texMods[tm],
									 matrix );
			break;

		case TMOD_ROTATE:
			RB_CalcRotateTexMatrix( bundle->texMods[tm].rotateSpeed,
									matrix );
			break;

		default:
			ri.Error( ERR_DROP, "ERROR: unknown texmod '%d' in shader '%s'", bundle->texMods[tm].type, tess.shader->name );
			break;
		}

		switch ( bundle->texMods[tm].type )
		{	
		case TMOD_TURBULENT:
			outMatrix[tm*2+0][0] = 1; outMatrix[tm*2+0][1] = 0; outMatrix[tm*2+0][2] = 0;
			outMatrix[tm*2+1][0] = 0; outMatrix[tm*2+1][1] = 1; outMatrix[tm*2+1][2] = 0;

			outMatrix[tm*2+0][3] = turb[0];
			outMatrix[tm*2+1][3] = turb[1];

			hasTurb = qtrue;
			break;

		case TMOD_NONE:
		case TMOD_ENTITY_TRANSLATE:
		case TMOD_SCROLL:
		case TMOD_SCALE:
		case TMOD_STRETCH:
		case TMOD_TRANSFORM:
		case TMOD_ROTATE:
		default:
			outMatrix[tm*2+0][0] = matrix[0]; outMatrix[tm*2+0][1] = matrix[2]; outMatrix[tm*2+0][2] = matrix[4];
			outMatrix[tm*2+1][0] = matrix[1]; outMatrix[tm*2+1][1] = matrix[3]; outMatrix[tm*2+1][2] = matrix[5];

			outMatrix[tm*2+0][3] = 0;
			outMatrix[tm*2+1][3] = 0;

			tmpmatrix[0] = matrix[0] * currentmatrix[0] + matrix[2] * currentmatrix[1];
			tmpmatrix[1] = matrix[1] * currentmatrix[0] + matrix[3] * currentmatrix[1];

			tmpmatrix[2] = matrix[0] * currentmatrix[2] + matrix[2] * currentmatrix[3];
			tmpmatrix[3] = matrix[1] * currentmatrix[2] + matrix[3] * currentmatrix[3];

			tmpmatrix[4] = matrix[0] * currentmatrix[4] + matrix[2] * currentmatrix[5] + matrix[4];
			tmpmatrix[5] = matrix[1] * currentmatrix[4] + matrix[3] * currentmatrix[5] + matrix[5];

			currentmatrix[0] = tmpmatrix[0];
			currentmatrix[1] = tmpmatrix[1];
			currentmatrix[2] = tmpmatrix[2];
			currentmatrix[3] = tmpmatrix[3];
			currentmatrix[4] = tmpmatrix[4];
			currentmatrix[5] = tmpmatrix[5];
			break;
		}
	}

	// if turb isn't used, only one matrix is needed
	if ( !hasTurb ) {
		tm = 0;

		outMatrix[tm*2+0][0] = currentmatrix[0]; outMatrix[tm*2+0][1] = currentmatrix[2]; outMatrix[tm*2+0][2] = currentmatrix[4];
		outMatrix[tm*2+1][0] = currentmatrix[1]; outMatrix[tm*2+1][1] = currentmatrix[3]; outMatrix[tm*2+1][2] = currentmatrix[5];

		outMatrix[tm*2+0][3] = 0;
		outMatrix[tm*2+1][3] = 0;
		tm++;
	}

	for ( ; tm < TR_MAX_TEXMODS ; tm++ ) {
		outMatrix[tm*2+0][0] = 1; outMatrix[tm*2+0][1] = 0; outMatrix[tm*2+0][2] = 0;
		outMatrix[tm*2+1][0] = 0; outMatrix[tm*2+1][1] = 1; outMatrix[tm*2+1][2] = 0;

		outMatrix[tm*2+0][3] = 0;
		outMatrix[tm*2+1][3] = 0;
	}
}


static void ComputeDeformValues(int *deformGen, vec5_t deformParams)
{
	// u_DeformGen
	*deformGen = DGEN_NONE;
	if(!ShaderRequiresCPUDeforms(tess.shader))
	{
		deformStage_t  *ds;

		// only support the first one
		ds = &tess.shader->deforms[0];

		switch (ds->deformation)
		{
			case DEFORM_WAVE:
				*deformGen = ds->deformationWave.func;

				deformParams[0] = ds->deformationWave.base;
				deformParams[1] = ds->deformationWave.amplitude;
				deformParams[2] = ds->deformationWave.phase;
				deformParams[3] = ds->deformationWave.frequency;
				deformParams[4] = ds->deformationSpread;
				break;

			case DEFORM_BULGE:
				*deformGen = DGEN_BULGE;

				deformParams[0] = 0;
				deformParams[1] = ds->bulgeHeight; // amplitude
				deformParams[2] = ds->bulgeWidth;  // phase
				deformParams[3] = ds->bulgeSpeed;  // frequency
				deformParams[4] = 0;
				break;

			default:
				break;
		}
	}
}


static void ProjectDlightTexture( void ) {
	int		l;
	vec3_t	origin;
	float	scale;
	float	radius;
	int deformGen;
	vec5_t deformParams;
	float intensity;
	qboolean vertexLight;
	int shaderNum;

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}

	ComputeDeformValues(&deformGen, deformParams);

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		scale = 1.0f / radius;
		intensity = dl->intensity;

		vertexLight = ( ( dl->flags & REF_DIRECTED_DLIGHT ) || ( dl->flags & REF_VERTEX_DLIGHT ) );

		shaderNum = (deformGen == DGEN_NONE) ? 0 : 1;

		sp = &tr.dlightShader[shaderNum];

		backEnd.pc.c_dlightDraws++;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);
		
		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);

			if (tess.shader->deforms[0].deformationWave.frequency < 0)
			{
				vec3_t worldUp;
				vec3_t fireRiseDir = { 0, 0, 1 };

				if ( !VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
					VectorCopy( backEnd.currentEntity->e.fireRiseDir, fireRiseDir );
				}

				if ( backEnd.currentEntity != &tr.worldEntity ) {    // world surfaces dont have an axis
					VectorRotate( fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
				} else {
					VectorCopy( fireRiseDir, worldUp );
				}

				GLSL_SetUniformVec3(sp, UNIFORM_FIRERISEDIR, worldUp);
			}
		}

		if ( dl->flags & REF_DIRECTED_DLIGHT ) {
			VectorCopy( dl->origin, origin );

			scale = (tess.shader->cullType == CT_TWO_SIDED);

			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, -1);
		} else if ( dl->flags & REF_VERTEX_DLIGHT ) {
			scale = dl->radiusInverseCubed;

			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);
		} else {
			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, 0);
		}

		vector[0] = dl->color[0];
		vector[1] = dl->color[1];
		vector[2] = dl->color[2];
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_COLOR, vector);

		vector[0] = origin[0];
		vector[1] = origin[1];
		vector[2] = origin[2];
		vector[3] = scale;
		GLSL_SetUniformVec4(sp, UNIFORM_DLIGHTINFO, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_INTENSITY, intensity);

		if ( dl->dlshader ) {
			shader_t *dls = dl->dlshader;
			int i;

			for ( i = 0; i < dls->numUnfoggedPasses; i++ ) {
				shaderStage_t *stage = dls->stages[i];
				R_BindAnimatedImageToTMU( &dls->stages[i]->bundle[0], TB_COLORMAP );
				GL_State( stage->stateBits | GLS_DEPTHFUNC_EQUAL );

				// alpha test function
				switch ( stage->stateBits & GLS_ATEST_FUNC_BITS )
				{
					case GLS_ATEST_GREATER:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_GREATER);
						break;
					case GLS_ATEST_LESS:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_LESS);
						break;
					case GLS_ATEST_GREATEREQUAL:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_GREATEREQUAL);
						break;
					case GLS_ATEST_LESSEQUAL:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_LESSEQUAL);
						break;
					case GLS_ATEST_EQUAL:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_EQUAL);
						break;
					case GLS_ATEST_NOTEQUAL:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_NOTEQUAL);
						break;
					default:
						GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_NONE);
						break;
				}

				// alpha test reference value
				GLSL_SetUniformFloat(sp, UNIFORM_ALPHATESTREF, ( ( stage->stateBits & GLS_ATEST_REF_BITS ) >> GLS_ATEST_REF_SHIFT ) / 100.0f);

				R_DrawElements(tess.numIndexes, tess.firstIndex);

				backEnd.pc.c_totalIndexes += tess.numIndexes;
				backEnd.pc.c_dlightIndexes += tess.numIndexes;
				backEnd.pc.c_dlightVertexes += tess.numVertexes;
			}
		} else {
			if ( vertexLight )
				GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
			else
				GL_BindToTMU( tr.dlightImage, TB_COLORMAP );

			// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
			// where they aren't rendered
			if ( dl->flags & REF_ADDITIVE_DLIGHT ) {
				GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
			}
			else {
				GL_State( GLS_ATEST_GT_0 | GLS_SRCBLEND_DST_COLOR | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
			}

			// alpha test function
			switch ( glState.glStateBits & GLS_ATEST_FUNC_BITS )
			{
				case GLS_ATEST_GREATER:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_GREATER);
					break;
				case GLS_ATEST_LESS:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_LESS);
					break;
				case GLS_ATEST_GREATEREQUAL:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_GREATEREQUAL);
					break;
				case GLS_ATEST_LESSEQUAL:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_LESSEQUAL);
					break;
				case GLS_ATEST_EQUAL:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_EQUAL);
					break;
				case GLS_ATEST_NOTEQUAL:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_NOTEQUAL);
					break;
				default:
					GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_NONE);
					break;
			}

			// alpha test reference value
			GLSL_SetUniformFloat(sp, UNIFORM_ALPHATESTREF, ( ( glState.glStateBits & GLS_ATEST_REF_BITS ) >> GLS_ATEST_REF_SHIFT ) / 100.0f);

			R_DrawElements(tess.numIndexes, tess.firstIndex);

			backEnd.pc.c_totalIndexes += tess.numIndexes;
			backEnd.pc.c_dlightIndexes += tess.numIndexes;
			backEnd.pc.c_dlightVertexes += tess.numVertexes;
		}
	}
}


static void ComputeShaderColors( shaderStage_t *pStage, vec4_t baseColor, vec4_t vertColor, int blend )
{
	qboolean isBlend = ((blend & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_DST_COLOR)
		|| ((blend & GLS_SRCBLEND_BITS) == GLS_SRCBLEND_ONE_MINUS_DST_COLOR)
		|| ((blend & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_SRC_COLOR)
		|| ((blend & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);

	qboolean is2DDraw = backEnd.currentEntity == &backEnd.entity2D;

	float overbright = (isBlend || is2DDraw) ? 1.0f : (float)(1 << tr.overbrightBits);

	baseColor[0] = 
	baseColor[1] =
	baseColor[2] =
	baseColor[3] = 1.0f;

	vertColor[0] =
	vertColor[1] =
	vertColor[2] =
	vertColor[3] = 0.0f;

	//
	// rgbGen
	//
	switch ( pStage->rgbGen )
	{
		case CGEN_EXACT_VERTEX:
		case CGEN_EXACT_VERTEX_LIT:
			baseColor[0] = 
			baseColor[1] =
			baseColor[2] = 
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] = overbright;
			vertColor[3] = 1.0f;
			break;
		case CGEN_CONST:
			baseColor[0] = pStage->constantColor[0] / 255.0f;
			baseColor[1] = pStage->constantColor[1] / 255.0f;
			baseColor[2] = pStage->constantColor[2] / 255.0f;
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			break;
		case CGEN_VERTEX:
		case CGEN_VERTEX_LIT:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] =
			baseColor[3] = 0.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] =
			vertColor[3] = 1.0f;
			break;
		case CGEN_ONE_MINUS_VERTEX:
			baseColor[0] = 
			baseColor[1] =
			baseColor[2] = 1.0f;

			vertColor[0] =
			vertColor[1] =
			vertColor[2] = -1.0f;
			break;
		case CGEN_FOG:
			{
				fog_t		*fog;
				unsigned	colorInt;

				if ( tess.shader->isSky ) {
					colorInt = tr.skyFogColorInt;
				} else {
					fog = tr.world->fogs + tess.fogNum;

					if ( fog->originalBrushNumber < 0 ) {
						colorInt = backEnd.refdef.fogColorInt;
					} else {
						colorInt = fog->colorInt;
					}
				}

				baseColor[0] = ((unsigned char *)(&colorInt))[0] / 255.0f;
				baseColor[1] = ((unsigned char *)(&colorInt))[1] / 255.0f;
				baseColor[2] = ((unsigned char *)(&colorInt))[2] / 255.0f;
				baseColor[3] = ((unsigned char *)(&colorInt))[3] / 255.0f;
			}
			break;
		case CGEN_WAVEFORM:
			baseColor[0] = 
			baseColor[1] = 
			baseColor[2] = RB_CalcWaveColorSingle( &pStage->rgbWave );
			break;
		case CGEN_COLOR_WAVEFORM:
			{
				float glow = RB_CalcWaveColorSingle( &pStage->rgbWave );

				baseColor[0] = glow * pStage->constantColor[0] / 255.0f;
				baseColor[1] = glow * pStage->constantColor[1] / 255.0f;
				baseColor[2] = glow * pStage->constantColor[2] / 255.0f;
			}
			break;
		case CGEN_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[0] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[0] / 255.0f;
				baseColor[1] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[1] / 255.0f;
				baseColor[2] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[2] / 255.0f;
				baseColor[3] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			break;
		case CGEN_ONE_MINUS_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[0] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[0] / 255.0f;
				baseColor[1] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[1] / 255.0f;
				baseColor[2] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[2] / 255.0f;
				baseColor[3] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			break;
		case CGEN_IDENTITY:
		case CGEN_LIGHTING_DIFFUSE:
		case CGEN_LIGHTING_DIFFUSE_ENTITY:
			baseColor[0] =
			baseColor[1] =
			baseColor[2] = overbright;
			break;
		case CGEN_IDENTITY_LIGHTING:
		case CGEN_BAD:
			break;
	}

	//
	// alphaGen
	//
	switch ( pStage->alphaGen )
	{
		case AGEN_SKIP:
			break;
		case AGEN_CONST:
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			vertColor[3] = 0.0f;
			break;
		case AGEN_WAVEFORM:
			baseColor[3] = RB_CalcWaveAlphaSingle( &pStage->alphaWave );
			vertColor[3] = 0.0f;
			break;
		case AGEN_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[3] = ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
		case AGEN_ONE_MINUS_ENTITY:
			if (backEnd.currentEntity)
			{
				baseColor[3] = 1.0f - ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
		case AGEN_VERTEX:
			baseColor[3] = 0.0f;
			vertColor[3] = 1.0f;
			break;
		case AGEN_ONE_MINUS_VERTEX:
			baseColor[3] = 1.0f;
			vertColor[3] = -1.0f;
			break;
		case AGEN_IDENTITY:
		case AGEN_LIGHTING_SPECULAR:
		case AGEN_PORTAL:
			// Done entirely in vertex program
			baseColor[3] = 1.0f;
			vertColor[3] = 0.0f;
			break;
		case AGEN_SKY_ALPHA:
			baseColor[3] = backEnd.refdef.skyAlpha;
			vertColor[3] = 0.0f;
			break;
		case AGEN_ONE_MINUS_SKY_ALPHA:
			baseColor[3] = 1.0f - backEnd.refdef.skyAlpha;
			vertColor[3] = 0.0f;
			break;
		case AGEN_NORMALZFADE:
			baseColor[3] = pStage->constantColor[3] / 255.0f;
			if (backEnd.currentEntity && backEnd.currentEntity->e.hModel)
			{
				baseColor[3] *= ((unsigned char *)backEnd.currentEntity->e.shaderRGBA)[3] / 255.0f;
			}
			vertColor[3] = 0.0f;
			break;
	}

	// FIXME: find some way to implement this.
#if 0
	// if in greyscale rendering mode turn all color values into greyscale.
	if(r_greyscale->integer)
	{
		int scale;
		
		for(i = 0; i < tess.numVertexes; i++)
		{
			scale = (tess.svars.colors[i][0] + tess.svars.colors[i][1] + tess.svars.colors[i][2]) / 3;
			tess.svars.colors[i][0] = tess.svars.colors[i][1] = tess.svars.colors[i][2] = scale;
		}
	}
#endif
}


static void ComputeFogValues(vec4_t fogDistanceVector, vec4_t fogDepthVector, float *eyeT, fogType_t *outFogType)
{
	// from RB_CalcFogTexCoords()
	fog_t  *fog;
	bmodel_t *bmodel;
	vec3_t  local;
	float tcScale;
	fogType_t fogType;

	if (!tess.fogNum) {
		if ( outFogType ) {
			*outFogType = FT_NONE;
		}
		return;
	}

	if ( tess.shader->isSky ) {
		fog = NULL;
		bmodel = NULL;
		tcScale = tr.skyFogTcScale;
		fogType = tr.skyFogType;
	} else {
		fog = tr.world->fogs + tess.fogNum;
		bmodel = tr.world->bmodels + fog->modelNum;

		// Global fog
		if ( fog->originalBrushNumber < 0 ) {
			if ( backEnd.refdef.fogType == FT_NONE ) {
				return;
			}

			tcScale = backEnd.refdef.fogTcScale;
			fogType = backEnd.refdef.fogType;
		} else {
			tcScale = fog->tcScale;
			fogType = fog->shader->fogParms.fogType;
		}
	}

	if ( outFogType ) {
		*outFogType = fogType;
	}

	if ( fogType == FT_NONE ) {
		return;
	}

	VectorSubtract( backEnd.or.origin, backEnd.viewParms.or.origin, local );
	fogDistanceVector[0] = -backEnd.or.modelMatrix[2];
	fogDistanceVector[1] = -backEnd.or.modelMatrix[6];
	fogDistanceVector[2] = -backEnd.or.modelMatrix[10];
	fogDistanceVector[3] = DotProduct( local, backEnd.viewParms.or.axis[0] );

	// scale the fog vectors based on the fog's thickness
	VectorScale4(fogDistanceVector, tcScale, fogDistanceVector);

	// rotate the gradient vector for this orientation
	if ( fog && fog->hasSurface ) {
		vec4_t fogSurface;

		// offset fog surface
		VectorCopy( fog->surface, fogSurface );
#if 1 // WolfET
		fogSurface[ 3 ] = fog->surface[ 3 ] + DotProduct( fogSurface, bmodel->orientation.origin );
#else
		fogSurface[ 3 ] = fog->surface[ 3 ];
#endif

		fogDepthVector[0] = fogSurface[0] * backEnd.or.axis[0][0] + 
			fog->surface[1] * backEnd.or.axis[0][1] + fog->surface[2] * backEnd.or.axis[0][2];
		fogDepthVector[1] = fogSurface[0] * backEnd.or.axis[1][0] + 
			fog->surface[1] * backEnd.or.axis[1][1] + fog->surface[2] * backEnd.or.axis[1][2];
		fogDepthVector[2] = fogSurface[0] * backEnd.or.axis[2][0] + 
			fog->surface[1] * backEnd.or.axis[2][1] + fog->surface[2] * backEnd.or.axis[2][2];
		fogDepthVector[3] = -fogSurface[3] + DotProduct( backEnd.or.origin, fog->surface );

		*eyeT = DotProduct( backEnd.or.viewOrigin, fogDepthVector ) + fogDepthVector[3];
	} else {
		*eyeT = 1;	// non-surface fog always has eye inside
	}
}


static void ComputeFogColorMask( shaderStage_t *pStage, vec4_t fogColorMask )
{
	switch(pStage->adjustColorsForFog)
	{
		case ACFF_MODULATE_RGB:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] = 1.0f;
			fogColorMask[3] = 0.0f;
			break;
		case ACFF_MODULATE_ALPHA:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] = 0.0f;
			fogColorMask[3] = 1.0f;
			break;
		case ACFF_MODULATE_RGBA:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] =
			fogColorMask[3] = 1.0f;
			break;
		default:
			fogColorMask[0] =
			fogColorMask[1] =
			fogColorMask[2] =
			fogColorMask[3] = 0.0f;
			break;
	}
}


static void ForwardDlight( void ) {
	int		l;
	//vec3_t	origin;
	//float	scale;
	float	radius;
	float	intensity;

	int deformGen;
	vec5_t deformParams;
	
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;

	shaderCommands_t *input = &tess;
	shaderStage_t *pStage = tess.xstages[0];

	if ( !backEnd.refdef.num_dlights ) {
		return;
	}
	
	ComputeDeformValues(&deformGen, deformParams);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT, NULL);

	for ( l = 0 ; l < backEnd.refdef.num_dlights ; l++ ) {
		dlight_t	*dl;
		shaderProgram_t *sp;
		vec4_t vector;
		vec4_t texMatrix[8];

		if ( !( tess.dlightBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this light
		}

		dl = &backEnd.refdef.dlights[l];
		//VectorCopy( dl->transformed, origin );
		radius = dl->radius;
		//scale = 1.0f / radius;
		intensity = dl->intensity;

		//if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			index &= ~LIGHTDEF_LIGHTTYPE_MASK;
			index |= LIGHTDEF_USE_LIGHT_VECTOR;

			sp = &tr.lightallShader[index];
		}

		backEnd.pc.c_lightallDraws++;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_LOCALVIEWORIGIN, backEnd.or.viewOrigin);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if ((deformGen != DGEN_NONE && tess.shader->deforms[0].deformationWave.frequency < 0 )
			|| pStage->alphaGen == AGEN_NORMALZFADE)
		{
			vec3_t worldUp;
			vec3_t fireRiseDir = { 0, 0, 1 };

			if ( !VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
				VectorCopy( backEnd.currentEntity->e.fireRiseDir, fireRiseDir );
			}

			if ( backEnd.currentEntity != &tr.worldEntity ) {    // world surfaces dont have an axis
				VectorRotate( fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
			} else {
				VectorCopy( fireRiseDir, worldUp );
			}

			GLSL_SetUniformVec3(sp, UNIFORM_FIRERISEDIR, worldUp);
		}

		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		if ( input->fogNum && ( !input->shader->noFog || pStage->isFogged ) ) {
			vec4_t fogColorMask;

			GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
			GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);

			ComputeFogColorMask(pStage, fogColorMask);

			GLSL_SetUniformVec4(sp, UNIFORM_FOGCOLORMASK, fogColorMask);
		}

		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors(pStage, baseColor, vertColor, GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE);

			GLSL_SetUniformVec4(sp, UNIFORM_BASECOLOR, baseColor);
			GLSL_SetUniformVec4(sp, UNIFORM_VERTCOLOR, vertColor);
		}

		if (pStage->alphaGen == AGEN_PORTAL)
		{
			GLSL_SetUniformFloat(sp, UNIFORM_PORTALRANGE, tess.shader->portalRange);
		}
		else if (pStage->alphaGen == AGEN_NORMALZFADE)
		{
			float lowest, highest;
			//qboolean zombieEffect = qfalse;

			lowest = pStage->zFadeBounds[0];
			if ( lowest == -1000 ) {    // use entity alpha
				lowest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}
			highest = pStage->zFadeBounds[1];
			if ( highest == -1000 ) {   // use entity alpha
				highest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}

			// TODO: Handle normalzfade zombie effect

			GLSL_SetUniformFloat(sp, UNIFORM_ZFADELOWEST, lowest);
			GLSL_SetUniformFloat(sp, UNIFORM_ZFADEHIGHEST, highest);
		}

		GLSL_SetUniformInt(sp, UNIFORM_COLORGEN, pStage->rgbGen);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHAGEN, pStage->alphaGen);

		if (pStage->bundle[0].tcGen == TCGEN_ENVIRONMENT_CELSHADE_MAPPED)
		{
			GLSL_SetUniformVec3(sp, UNIFORM_MODELLIGHTDIR, backEnd.currentEntity->modelLightDir);
		}

		intensity = Com_Clamp(0.0f, 1.0f, intensity);
		VectorScale(dl->color, intensity, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, vector);

		VectorSet(vector, 0, 0, 0);
		GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT, vector);

		VectorCopy(dl->origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);

		GLSL_SetUniformVec4(sp, UNIFORM_NORMALSCALE, pStage->normalScale);
		GLSL_SetUniformVec4(sp, UNIFORM_SPECULARSCALE, pStage->specularScale);
		
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);
		GLSL_SetUniformFloat(sp, UNIFORM_ALPHATESTREF, 0.0f);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix);

		if (pStage->bundle[TB_DIFFUSEMAP].image[0])
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP);

		// bind textures that are sampled and used in the glsl shader, and
		// bind whiteImage to textures that are sampled but zeroed in the glsl shader
		//
		// alternatives:
		//  - use the last bound texture
		//     -> costs more to sample a higher res texture then throw out the result
		//  - disable texture sampling in glsl shader with #ifdefs, as before
		//     -> increases the number of shaders that must be compiled
		//

		if (pStage->bundle[TB_NORMALMAP].image[0])
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP);
		}
		else if (r_normalMapping->integer)
			GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );

		if (pStage->bundle[TB_SPECULARMAP].image[0])
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP);
		}
		else if (r_specularMapping->integer)
			GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );

		{
			vec4_t enableTextures;

			VectorSet4(enableTextures, 0.0f, 0.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(sp, UNIFORM_ENABLETEXTURES, enableTextures);
		}

		if (r_dlightMode->integer >= 2)
			GL_BindToTMU(tr.shadowCubemaps[l], TB_SHADOWMAP);

		ComputeTexMods( pStage, TB_DIFFUSEMAP, texMatrix );
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6]);
		GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7]);

		GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen);

		//
		// draw
		//

		R_DrawElements(input->numIndexes, input->firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		backEnd.pc.c_dlightIndexes += tess.numIndexes;
		backEnd.pc.c_dlightVertexes += tess.numVertexes;
	}
}


static void ProjectPshadowVBOGLSL( void ) {
	int		l;
	vec3_t	origin;
	float	radius;

	int deformGen;
	vec5_t deformParams;

	shaderCommands_t *input = &tess;

	if ( !backEnd.refdef.num_pshadows ) {
		return;
	}
	
	ComputeDeformValues(&deformGen, deformParams);

	for ( l = 0 ; l < backEnd.refdef.num_pshadows ; l++ ) {
		pshadow_t	*ps;
		shaderProgram_t *sp;
		vec4_t vector;

		if ( !( tess.pshadowBits & ( 1 << l ) ) ) {
			continue;	// this surface definitely doesn't have any of this shadow
		}

		ps = &backEnd.refdef.pshadows[l];
		VectorCopy( ps->lightOrigin, origin );
		radius = ps->lightRadius;

		sp = &tr.pshadowShader;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		VectorCopy(origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);

		VectorScale(ps->lightViewAxis[0], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTFORWARD, vector);

		VectorScale(ps->lightViewAxis[1], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTRIGHT, vector);

		VectorScale(ps->lightViewAxis[2], 1.0f / ps->viewRadius, vector);
		GLSL_SetUniformVec3(sp, UNIFORM_LIGHTUP, vector);

		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, radius);
	  
		// include GLS_DEPTHFUNC_EQUAL so alpha tested surfaces don't add light
		// where they aren't rendered
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);
		GLSL_SetUniformFloat(sp, UNIFORM_ALPHATESTREF, 0.0f);

		GL_BindToTMU( tr.pshadowMaps[l], TB_DIFFUSEMAP );

		//
		// draw
		//

		R_DrawElements(input->numIndexes, input->firstIndex);

		backEnd.pc.c_totalIndexes += tess.numIndexes;
		//backEnd.pc.c_dlightIndexes += tess.numIndexes;
	}
}



/*
===================
RB_FogPass

Blends a fog texture on top of everything else
===================
*/
static void RB_FogPass( void ) {
	fog_t		*fog;
	vec4_t  color;
	vec4_t	fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float	eyeT = 0;
	shaderProgram_t *sp;
	fogType_t	fogType;
	int		colorInt;

	int deformGen;
	vec5_t deformParams;

	if ( tess.shader->isSky ) {
		fogType = tr.skyFogType;
		colorInt = tr.skyFogColorInt;
	} else {
		fog = tr.world->fogs + tess.fogNum;

		// Global fog
		if ( fog->originalBrushNumber < 0 ) {
			fogType = backEnd.refdef.fogType;
			colorInt = backEnd.refdef.fogColorInt;
		} else {
			fogType = fog->shader->fogParms.fogType;
			colorInt = fog->colorInt;
		}
	}

	if ( fogType == FT_NONE ) {
		return;
	}

	// check if any stage is fogged
	if ( tess.shader->noFog ) {
		int i;

		for ( i = 0 ; i < MAX_SHADER_STAGES; i++ ) {
			shaderStage_t *pStage = tess.xstages[i];

			if ( !pStage ) {
				return;
			}

			if ( pStage->isFogged ) {
				break;
			}
		}
	}

	ComputeDeformValues(&deformGen, deformParams);

	{
		int index = 0;

		if (deformGen != DGEN_NONE)
			index |= FOGDEF_USE_DEFORM_VERTEXES;

		if (glState.vertexAnimation)
			index |= FOGDEF_USE_VERTEX_ANIMATION;
		else if (glState.boneAnimation)
			index |= FOGDEF_USE_BONE_ANIMATION;
		
		sp = &tr.fogShader[index];
	}

	backEnd.pc.c_fogDraws++;

	GLSL_BindProgram(sp);

	GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

	GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

	if (glState.boneAnimation)
	{
		GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
	}
	
	GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
	if (deformGen != DGEN_NONE)
	{
		GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
		GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);

		if (tess.shader->deforms[0].deformationWave.frequency < 0)
		{
			vec3_t worldUp;
			vec3_t fireRiseDir = { 0, 0, 1 };

			if ( !VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
				VectorCopy( backEnd.currentEntity->e.fireRiseDir, fireRiseDir );
			}

			if ( backEnd.currentEntity != &tr.worldEntity ) {    // world surfaces dont have an axis
				VectorRotate( fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
			} else {
				VectorCopy( fireRiseDir, worldUp );
			}

			GLSL_SetUniformVec3(sp, UNIFORM_FIRERISEDIR, worldUp);
		}
	}

	color[0] = ((unsigned char *)(&colorInt))[0] / 255.0f;
	color[1] = ((unsigned char *)(&colorInt))[1] / 255.0f;
	color[2] = ((unsigned char *)(&colorInt))[2] / 255.0f;
	color[3] = ((unsigned char *)(&colorInt))[3] / 255.0f;
	GLSL_SetUniformVec4(sp, UNIFORM_COLOR, color);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT, NULL);

	GLSL_SetUniformInt(sp, UNIFORM_FOGTYPE, fogType);
	GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
	GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
	GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);

	if ( tess.shader->fogPass == FP_EQUAL ) {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA | GLS_DEPTHFUNC_EQUAL );
	} else {
		GL_State( GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA );
	}
	GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);
	GLSL_SetUniformFloat(sp, UNIFORM_ALPHATESTREF, 0.0f);

	R_DrawElements(tess.numIndexes, tess.firstIndex);
}


static unsigned int RB_CalcShaderVertexAttribs( shaderCommands_t *input )
{
	unsigned int vertexAttribs = input->shader->vertexAttribs;

	if(glState.vertexAnimation)
	{
		vertexAttribs |= ATTR_POSITION2;
		if (vertexAttribs & ATTR_NORMAL)
		{
			vertexAttribs |= ATTR_NORMAL2;
			vertexAttribs |= ATTR_TANGENT2;
		}
	}

	return vertexAttribs;
}

static void RB_IterateStagesGeneric( shaderCommands_t *input )
{
	int stage;
	qboolean overridealpha = qfalse;
	int oldAlphaGen = AGEN_IDENTITY;
	int oldStateBits = 0;
	qboolean overridecolor = qfalse;
	int oldRgbGen = CGEN_IDENTITY;
	
	vec4_t fogDistanceVector, fogDepthVector = {0, 0, 0, 0};
	float eyeT = 0;
	fogType_t fogType = FT_NONE, stageFogType;

	int deformGen;
	vec5_t deformParams;

	qboolean renderToCubemap = tr.renderCubeFbo && glState.currentFBO == tr.renderCubeFbo;

	ComputeDeformValues(&deformGen, deformParams);

	ComputeFogValues(fogDistanceVector, fogDepthVector, &eyeT, &fogType);

	for ( stage = 0; stage < MAX_SHADER_STAGES; stage++ )
	{
		shaderStage_t *pStage = input->xstages[stage];
		shaderProgram_t *sp;
		vec4_t texMatrix[8];

		if ( !pStage )
		{
			break;
		}

		// override the shader alpha channel if requested
		if ( backEnd.currentEntity && backEnd.currentEntity->e.renderfx & RF_FORCE_ENT_ALPHA )
		{
			overridealpha = qtrue;
			oldAlphaGen = pStage->alphaGen;
			oldStateBits = pStage->stateBits;
			pStage->alphaGen = AGEN_ENTITY;

			// set bits for blendfunc blend
			pStage->stateBits = GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;

			// keep the original alphafunc, if any
			pStage->stateBits |= ( oldStateBits & GLS_ATEST_BITS );
		}
		else
		{
			overridealpha = qfalse;
		}

		// override the shader color channels if requested
		if ( backEnd.currentEntity && backEnd.currentEntity->e.renderfx & RF_RGB_TINT )
		{
			overridecolor = qtrue;
			oldRgbGen = pStage->rgbGen;
			pStage->rgbGen = CGEN_ENTITY;
		}
		else
		{
			overridecolor = qfalse;
		}

		stageFogType = FT_NONE;

		if (backEnd.depthFill)
		{
			if (pStage->glslShaderGroup == tr.lightallShader)
			{
				int index = 0;

				if (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity)
				{
					if (glState.boneAnimation)
					{
						index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
					}
					else
					{
						index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
					}
				}

				if (pStage->stateBits & GLS_ATEST_BITS)
				{
					index |= LIGHTDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &pStage->glslShaderGroup[index];
			}
			else
			{
				int shaderAttribs = 0;

				if (tess.shader->numDeforms && !ShaderRequiresCPUDeforms(tess.shader))
				{
					shaderAttribs |= GENERICDEF_USE_DEFORM_VERTEXES;
				}

				if (glState.vertexAnimation)
				{
					shaderAttribs |= GENERICDEF_USE_VERTEX_ANIMATION;
				}
				else if (glState.boneAnimation)
				{
					shaderAttribs |= GENERICDEF_USE_BONE_ANIMATION;
				}

				if (pStage->stateBits & GLS_ATEST_BITS)
				{
					shaderAttribs |= GENERICDEF_USE_TCGEN_AND_TCMOD;
				}

				sp = &tr.genericShader[shaderAttribs];
			}
		}
		else if (pStage->glslShaderGroup == tr.lightallShader)
		{
			int index = pStage->glslShaderIndex;

			if (backEnd.currentEntity && backEnd.currentEntity != &tr.worldEntity)
			{
				if (glState.boneAnimation)
				{
					index |= LIGHTDEF_ENTITY_BONE_ANIMATION;
				}
				else
				{
					index |= LIGHTDEF_ENTITY_VERTEX_ANIMATION;
				}
			}

			if (r_sunlightMode->integer && (backEnd.viewParms.flags & VPF_USESUNLIGHT) && (index & LIGHTDEF_LIGHTTYPE_MASK))
			{
				index |= LIGHTDEF_USE_SHADOWMAP;
			}

			if (r_lightmap->integer && ((index & LIGHTDEF_LIGHTTYPE_MASK) == LIGHTDEF_USE_LIGHTMAP))
			{
				index = LIGHTDEF_USE_TCGEN_AND_TCMOD;
			}

			sp = &pStage->glslShaderGroup[index];

			backEnd.pc.c_lightallDraws++;
		}
		else
		{
			stageFogType = ( !input->shader->noFog || pStage->isFogged ) ? fogType : FT_NONE;

			sp = GLSL_GetGenericShaderProgram(stage, stageFogType);

			backEnd.pc.c_genericDraws++;
		}

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);
		GLSL_SetUniformVec3(sp, UNIFORM_VIEWORIGIN, backEnd.viewParms.or.origin);
		GLSL_SetUniformVec3(sp, UNIFORM_LOCALVIEWORIGIN, backEnd.or.viewOrigin);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if (glState.boneAnimation)
		{
			GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
		}

		if ((deformGen != DGEN_NONE && tess.shader->deforms[0].deformationWave.frequency < 0 )
			|| pStage->alphaGen == AGEN_NORMALZFADE)
		{
			vec3_t worldUp;
			vec3_t fireRiseDir = { 0, 0, 1 };

			if ( !VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
				VectorCopy( backEnd.currentEntity->e.fireRiseDir, fireRiseDir );
			}

			if ( backEnd.currentEntity != &tr.worldEntity ) {    // world surfaces dont have an axis
				VectorRotate( fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
			} else {
				VectorCopy( fireRiseDir, worldUp );
			}

			GLSL_SetUniformVec3(sp, UNIFORM_FIRERISEDIR, worldUp);
		}
		
		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);
		}

		GLSL_SetUniformInt(sp, UNIFORM_FOGTYPE, stageFogType);
		if (stageFogType != FT_NONE)
		{
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDISTANCE, fogDistanceVector);
			GLSL_SetUniformVec4(sp, UNIFORM_FOGDEPTH, fogDepthVector);
			GLSL_SetUniformFloat(sp, UNIFORM_FOGEYET, eyeT);
		}

		GL_State( pStage->stateBits );

		// alpha test function
		switch ( pStage->stateBits & GLS_ATEST_FUNC_BITS )
		{
			case GLS_ATEST_GREATER:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_GREATER);
				break;
			case GLS_ATEST_LESS:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_LESS);
				break;
			case GLS_ATEST_GREATEREQUAL:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_GREATEREQUAL);
				break;
			case GLS_ATEST_LESSEQUAL:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_LESSEQUAL);
				break;
			case GLS_ATEST_EQUAL:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_EQUAL);
				break;
			case GLS_ATEST_NOTEQUAL:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_NOTEQUAL);
				break;
			default:
				GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, U_ATEST_NONE);
				break;
		}

		// alpha test reference value
		GLSL_SetUniformFloat(sp, UNIFORM_ALPHATESTREF, ( ( pStage->stateBits & GLS_ATEST_REF_BITS ) >> GLS_ATEST_REF_SHIFT ) / 100.0f);


		{
			vec4_t baseColor;
			vec4_t vertColor;

			ComputeShaderColors(pStage, baseColor, vertColor, pStage->stateBits);

			GLSL_SetUniformVec4(sp, UNIFORM_BASECOLOR, baseColor);
			GLSL_SetUniformVec4(sp, UNIFORM_VERTCOLOR, vertColor);
		}

		if (pStage->rgbGen == CGEN_LIGHTING_DIFFUSE || pStage->rgbGen == CGEN_LIGHTING_DIFFUSE_ENTITY)
		{
			vec4_t vec;

			VectorScale(backEnd.currentEntity->ambientLight, 1.0f / 255.0f, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_AMBIENTLIGHT, vec);

			VectorScale(backEnd.currentEntity->directedLight, 1.0f / 255.0f, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_DIRECTEDLIGHT, vec);
			
			VectorCopy(backEnd.currentEntity->lightDir, vec);
			vec[3] = 0.0f;
			GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vec);
			GLSL_SetUniformVec3(sp, UNIFORM_MODELLIGHTDIR, backEnd.currentEntity->modelLightDir);

			GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, 0.0f);

			if ( pStage->rgbGen == CGEN_LIGHTING_DIFFUSE_ENTITY )
			{
				int i;

				for ( i = 0; i < 3; ++i )
				{
					vec[i] = backEnd.currentEntity->e.shaderRGBA[i] / 255.0f;
				}

				GLSL_SetUniformVec3(sp, UNIFORM_DIFFUSECOLOR, vec);
			}
		}
		else if (pStage->bundle[0].tcGen == TCGEN_ENVIRONMENT_CELSHADE_MAPPED)
		{
			GLSL_SetUniformVec3(sp, UNIFORM_MODELLIGHTDIR, backEnd.currentEntity->modelLightDir);
		}

		if (pStage->alphaGen == AGEN_PORTAL)
		{
			GLSL_SetUniformFloat(sp, UNIFORM_PORTALRANGE, tess.shader->portalRange);
		}
		else if (pStage->alphaGen == AGEN_NORMALZFADE)
		{
			float lowest, highest;
			//qboolean zombieEffect = qfalse;

			lowest = pStage->zFadeBounds[0];
			if ( lowest == -1000 ) {    // use entity alpha
				lowest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}
			highest = pStage->zFadeBounds[1];
			if ( highest == -1000 ) {   // use entity alpha
				highest = backEnd.currentEntity->e.shaderTime;
				//zombieEffect = qtrue;
			}

			// TODO: Handle normalzfade zombie effect

			GLSL_SetUniformFloat(sp, UNIFORM_ZFADELOWEST, lowest);
			GLSL_SetUniformFloat(sp, UNIFORM_ZFADEHIGHEST, highest);
		}

		GLSL_SetUniformInt(sp, UNIFORM_COLORGEN, pStage->rgbGen);
		GLSL_SetUniformInt(sp, UNIFORM_ALPHAGEN, pStage->alphaGen);

		if (stageFogType != FT_NONE)
		{
			vec4_t fogColorMask;

			ComputeFogColorMask(pStage, fogColorMask);

			GLSL_SetUniformVec4(sp, UNIFORM_FOGCOLORMASK, fogColorMask);
		}

		if (r_lightmap->integer)
		{
			vec4_t st[2];
			VectorSet4(st[0], 1.0f, 0.0f, 0.0f, 0.0f);
			VectorSet4(st[1], 0.0f, 1.0f, 0.0f, 0.0f);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, st[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, st[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, st[1]);

			GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, TCGEN_LIGHTMAP);
		}
		else
		{
			ComputeTexMods(pStage, TB_DIFFUSEMAP, texMatrix);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX0, texMatrix[0]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX1, texMatrix[1]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX2, texMatrix[2]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX3, texMatrix[3]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX4, texMatrix[4]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX5, texMatrix[5]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX6, texMatrix[6]);
			GLSL_SetUniformVec4(sp, UNIFORM_DIFFUSETEXMATRIX7, texMatrix[7]);

			GLSL_SetUniformInt(sp, UNIFORM_TCGEN0, pStage->bundle[0].tcGen);
			if (pStage->bundle[0].tcGen == TCGEN_VECTOR)
			{
				vec3_t vec;

				VectorCopy(pStage->bundle[0].tcGenVectors[0], vec);
				GLSL_SetUniformVec3(sp, UNIFORM_TCGEN0VECTOR0, vec);
				VectorCopy(pStage->bundle[0].tcGenVectors[1], vec);
				GLSL_SetUniformVec3(sp, UNIFORM_TCGEN0VECTOR1, vec);
			}
		}

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix);

		GLSL_SetUniformVec4(sp, UNIFORM_NORMALSCALE, pStage->normalScale);

		{
			vec4_t specularScale;
			Vector4Copy(pStage->specularScale, specularScale);

			if (renderToCubemap)
			{
				// force specular to nonmetal if rendering cubemaps
				if (r_pbr->integer)
					specularScale[1] = 0.0f;
			}

			GLSL_SetUniformVec4(sp, UNIFORM_SPECULARSCALE, specularScale);
		}

		//GLSL_SetUniformFloat(sp, UNIFORM_MAPLIGHTSCALE, backEnd.refdef.mapLightScale);

		//
		// do multitexture
		//
		if ( backEnd.depthFill )
		{
			if (!(pStage->stateBits & GLS_ATEST_BITS))
				GL_BindToTMU( tr.whiteImage, TB_COLORMAP );
			else if ( pStage->bundle[TB_COLORMAP].image[0] != 0 )
				R_BindAnimatedImageToTMU( &pStage->bundle[TB_COLORMAP], TB_COLORMAP );
		}
		else if ( pStage->glslShaderGroup == tr.lightallShader )
		{
			int i;
			vec4_t enableTextures;

			if (r_sunlightMode->integer && (backEnd.viewParms.flags & VPF_USESUNLIGHT) && (pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK))
			{
				// FIXME: screenShadowImage is NULL if no framebuffers
				if (tr.screenShadowImage)
					GL_BindToTMU(tr.screenShadowImage, TB_SHADOWMAP);
				GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTAMBIENT, backEnd.refdef.sunAmbCol);
				if (r_pbr->integer)
				{
					vec3_t color;

					color[0] = backEnd.refdef.sunCol[0] * backEnd.refdef.sunCol[0];
					color[1] = backEnd.refdef.sunCol[1] * backEnd.refdef.sunCol[1];
					color[2] = backEnd.refdef.sunCol[2] * backEnd.refdef.sunCol[2];
					GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, color);
				}
				else
				{
					GLSL_SetUniformVec3(sp, UNIFORM_PRIMARYLIGHTCOLOR, backEnd.refdef.sunCol);
				}
				GLSL_SetUniformVec4(sp, UNIFORM_PRIMARYLIGHTORIGIN,  backEnd.refdef.sunDir);
			}

			VectorSet4(enableTextures, 0, 0, 0, 0);
			if ((r_lightmap->integer == 1 || r_lightmap->integer == 2) && pStage->bundle[TB_LIGHTMAP].image[0])
			{
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if (i == TB_COLORMAP)
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], i);
					else
						GL_BindToTMU( tr.whiteImage, i );
				}
			}
			else if (r_lightmap->integer == 3 && pStage->bundle[TB_DELUXEMAP].image[0])
			{
				for (i = 0; i < NUM_TEXTURE_BUNDLES; i++)
				{
					if (i == TB_COLORMAP)
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], i);
					else
						GL_BindToTMU( tr.whiteImage, i );
				}
			}
			else
			{
				qboolean light = (pStage->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) != 0;
				qboolean fastLight = !(r_normalMapping->integer || r_specularMapping->integer);

				if (pStage->bundle[TB_DIFFUSEMAP].image[0])
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_DIFFUSEMAP], TB_DIFFUSEMAP);

				if (pStage->bundle[TB_LIGHTMAP].image[0])
					R_BindAnimatedImageToTMU( &pStage->bundle[TB_LIGHTMAP], TB_LIGHTMAP);

				// bind textures that are sampled and used in the glsl shader, and
				// bind whiteImage to textures that are sampled but zeroed in the glsl shader
				//
				// alternatives:
				//  - use the last bound texture
				//     -> costs more to sample a higher res texture then throw out the result
				//  - disable texture sampling in glsl shader with #ifdefs, as before
				//     -> increases the number of shaders that must be compiled
				//
				if (light && !fastLight)
				{
					if (pStage->bundle[TB_NORMALMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_NORMALMAP], TB_NORMALMAP);
						enableTextures[0] = 1.0f;
					}
					else if (r_normalMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_NORMALMAP );

					if (pStage->bundle[TB_DELUXEMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_DELUXEMAP], TB_DELUXEMAP);
						enableTextures[1] = 1.0f;
					}
					else if (r_deluxeMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_DELUXEMAP );

					if (pStage->bundle[TB_SPECULARMAP].image[0])
					{
						R_BindAnimatedImageToTMU( &pStage->bundle[TB_SPECULARMAP], TB_SPECULARMAP);
						enableTextures[2] = 1.0f;
					}
					else if (r_specularMapping->integer)
						GL_BindToTMU( tr.whiteImage, TB_SPECULARMAP );
				}

				enableTextures[3] = (r_cubeMapping->integer && !(tr.viewParms.flags & VPF_NOCUBEMAPS) && input->cubemapIndex) ? 1.0f : 0.0f;
			}

			GLSL_SetUniformVec4(sp, UNIFORM_ENABLETEXTURES, enableTextures);
		}
		else if ( pStage->bundle[1].image[0] != 0 )
		{
			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );

			//
			// lightmap/secondary pass
			//
			if ( r_lightmap->integer && pStage->bundle[1].isLightmap ) {
				GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1ENV, GL_REPLACE);
			} else {
				GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1ENV, pStage->multitextureEnv);
			}

			R_BindAnimatedImageToTMU( &pStage->bundle[1], 1 );
		}
		else 
		{
			//
			// set state
			//
			R_BindAnimatedImageToTMU( &pStage->bundle[0], 0 );

			GLSL_SetUniformInt(sp, UNIFORM_TEXTURE1ENV, 0);
		}

		//
		// testing cube map
		//
		if (!(tr.viewParms.flags & VPF_NOCUBEMAPS) && input->cubemapIndex && r_cubeMapping->integer)
		{
			vec4_t vec;
			cubemap_t *cubemap = &tr.cubemaps[input->cubemapIndex - 1];

			// FIXME: cubemap image could be NULL if cubemap isn't renderer or loaded
			if (cubemap->image)
				GL_BindToTMU( cubemap->image, TB_CUBEMAP);

			VectorSubtract(cubemap->origin, backEnd.viewParms.or.origin, vec);
			vec[3] = 1.0f;

			VectorScale4(vec, 1.0f / cubemap->parallaxRadius, vec);

			GLSL_SetUniformVec4(sp, UNIFORM_CUBEMAPINFO, vec);
		}

		//
		// draw
		//
		R_DrawElements(input->numIndexes, input->firstIndex);

		if ( overridealpha )
		{
			pStage->alphaGen = oldAlphaGen;
			pStage->stateBits = oldStateBits;
		}

		if ( overridecolor )
		{
			pStage->rgbGen = oldRgbGen;
		}

		// allow skipping out to show just lightmaps during development
		if ( r_lightmap->integer && ( pStage->bundle[0].isLightmap || pStage->bundle[1].isLightmap ) )
		{
			break;
		}

		if (backEnd.depthFill)
			break;
	}
}


static void RB_RenderShadowmap( shaderCommands_t *input )
{
	int deformGen;
	vec5_t deformParams;

	ComputeDeformValues(&deformGen, deformParams);

	{
		shaderProgram_t *sp = &tr.shadowmapShader[0];

		if (glState.vertexAnimation)
		{
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_VERTEX_ANIMATION];
		}
		else if (glState.boneAnimation)
		{
			sp = &tr.shadowmapShader[SHADOWMAPDEF_USE_BONE_ANIMATION];
		}

		vec4_t vector;

		GLSL_BindProgram(sp);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELVIEWPROJECTIONMATRIX, glState.modelviewProjection);

		GLSL_SetUniformMat4(sp, UNIFORM_MODELMATRIX, backEnd.or.transformMatrix);

		GLSL_SetUniformFloat(sp, UNIFORM_VERTEXLERP, glState.vertexAttribsInterpolation);

		if (glState.boneAnimation)
		{
			GLSL_SetUniformMat4BoneMatrix(sp, UNIFORM_BONEMATRIX, glState.boneMatrix, glState.boneAnimation);
		}

		GLSL_SetUniformInt(sp, UNIFORM_DEFORMGEN, deformGen);
		if (deformGen != DGEN_NONE)
		{
			GLSL_SetUniformFloat5(sp, UNIFORM_DEFORMPARAMS, deformParams);
			GLSL_SetUniformFloat(sp, UNIFORM_TIME, tess.shaderTime);

			if (tess.shader->deforms[0].deformationWave.frequency < 0)
			{
				vec3_t worldUp;
				vec3_t fireRiseDir = { 0, 0, 1 };

				if ( !VectorCompare( backEnd.currentEntity->e.fireRiseDir, vec3_origin ) ) {
					VectorCopy( backEnd.currentEntity->e.fireRiseDir, fireRiseDir );
				}

				if ( backEnd.currentEntity != &tr.worldEntity ) {    // world surfaces dont have an axis
					VectorRotate( fireRiseDir, backEnd.currentEntity->e.axis, worldUp );
				} else {
					VectorCopy( fireRiseDir, worldUp );
				}

				GLSL_SetUniformVec3(sp, UNIFORM_FIRERISEDIR, worldUp);
			}
		}

		VectorCopy(backEnd.viewParms.or.origin, vector);
		vector[3] = 1.0f;
		GLSL_SetUniformVec4(sp, UNIFORM_LIGHTORIGIN, vector);
		GLSL_SetUniformFloat(sp, UNIFORM_LIGHTRADIUS, backEnd.viewParms.zFar);

		GL_State( 0 );
		GLSL_SetUniformInt(sp, UNIFORM_ALPHATEST, 0);

		//
		// do multitexture
		//
		//if ( pStage->glslShaderGroup )
		{
			//
			// draw
			//

			R_DrawElements(input->numIndexes, input->firstIndex);
		}
	}
}



/*
** RB_StageIteratorGeneric
*/
void RB_StageIteratorGeneric( void )
{
	shaderCommands_t *input;
	unsigned int vertexAttribs = 0;

	input = &tess;
	
	if (!input->numVertexes || !input->numIndexes)
	{
		return;
	}

	if (tess.useInternalVao)
	{
		RB_DeformTessGeometry();
	}

	vertexAttribs = RB_CalcShaderVertexAttribs( input );

	if (tess.useInternalVao)
	{
		RB_UpdateTessVao(vertexAttribs);
	}
	else
	{
		backEnd.pc.c_staticVaoDraws++;
	}

	//
	// log this call
	//
	if ( r_logFile->integer ) 
	{
		// don't just call LogComment, or we will get
		// a call to va() every frame!
		GLimp_LogComment( va("--- RB_StageIteratorGeneric( %s ) ---\n", tess.shader->name) );
	}

	//
	// set face culling appropriately
	//
	if (input->shader->cullType == CT_TWO_SIDED)
	{
		GL_Cull( CT_TWO_SIDED );
	}
	else
	{
		qboolean cullFront = (input->shader->cullType == CT_FRONT_SIDED);

		if ( backEnd.viewParms.flags & VPF_DEPTHSHADOW )
			cullFront = !cullFront;

		if ( backEnd.viewParms.isMirror )
			cullFront = !cullFront;

		if ( backEnd.currentEntity && backEnd.currentEntity->mirrored )
			cullFront = !cullFront;

		if (cullFront)
			GL_Cull( CT_FRONT_SIDED );
		else
			GL_Cull( CT_BACK_SIDED );
	}

	// set polygon offset if necessary
	if ( input->shader->polygonOffset )
	{
		qglEnable( GL_POLYGON_OFFSET_FILL );
		qglPolygonOffset( r_offsetFactor->value, r_offsetUnits->value );
	}

	//
	// render depth if in depthfill mode
	//
	if (backEnd.depthFill)
	{
		RB_IterateStagesGeneric( input );

		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}

		return;
	}

	//
	// render shadowmap if in shadowmap mode
	//
	if (backEnd.viewParms.flags & VPF_SHADOWMAP)
	{
		if ( input->shader->sort == SS_OPAQUE )
		{
			RB_RenderShadowmap( input );
		}
		//
		// reset polygon offset
		//
		if ( input->shader->polygonOffset )
		{
			qglDisable( GL_POLYGON_OFFSET_FILL );
		}

		return;
	}

	//
	//
	// call shader function
	//
	RB_IterateStagesGeneric( input );

	//
	// pshadows!
	//
	if (glRefConfig.framebufferObject && r_shadows->integer == 4 && tess.pshadowBits
		&& tess.shader->sort <= SS_OPAQUE && !(tess.shader->surfaceParms & (SURF_NODLIGHT | SURF_SKY) ) ) {
		ProjectPshadowVBOGLSL();
	}


	// 
	// now do any dynamic lighting needed
	//
	if ( tess.dlightBits && tess.shader->sort <= SS_OPAQUE && r_lightmap->integer == 0
		&& !(tess.shader->surfaceParms & (SURF_NODLIGHT | SURF_SKY) ) ) {
		if (tess.shader->numUnfoggedPasses == 1 && tess.xstages[0]->glslShaderGroup == tr.lightallShader
			&& (tess.xstages[0]->glslShaderIndex & LIGHTDEF_LIGHTTYPE_MASK) && r_dlightMode->integer)
		{
			ForwardDlight();
		}
		else
		{
			ProjectDlightTexture();
		}
	}

	//
	// now do fog
	//
	if ( tess.fogNum && tess.shader->fogPass ) {
		RB_FogPass();
	}

	//
	// reset polygon offset
	//
	if ( input->shader->polygonOffset )
	{
		qglDisable( GL_POLYGON_OFFSET_FILL );
	}
}

/*
** RB_EndSurface
*/
void RB_EndSurface( void ) {
	shaderCommands_t *input;

	input = &tess;

	if (input->numIndexes == 0 || input->numVertexes == 0) {
		return;
	}

	if (input->indexes[SHADER_MAX_INDEXES-1] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_INDEXES hit");
	}	
	if (input->xyz[SHADER_MAX_VERTEXES-1][0] != 0) {
		ri.Error (ERR_DROP, "RB_EndSurface() - SHADER_MAX_VERTEXES hit");
	}

	if ( tess.shader == tr.shadowShader ) {
		RB_ShadowTessEnd();
		return;
	}

	// for debugging of sort order issues, stop rendering after a given sort value
	if ( r_debugSort->integer && r_debugSort->integer < tess.shader->sort ) {
		return;
	}

	if (tess.useCacheVao)
	{
		// upload indexes now
		VaoCache_Commit();
	}

	//
	// update performance counters
	//
	backEnd.pc.c_shaders++;
	backEnd.pc.c_vertexes += tess.numVertexes;
	backEnd.pc.c_indexes += tess.numIndexes;
	backEnd.pc.c_totalIndexes += tess.numIndexes * tess.numPasses;

	//
	// call off to shader specific tess end function
	//
	tess.currentStageIteratorFunc();

	//
	// draw debugging stuff
	//
	if ( r_showtris->integer ) {
		DrawTris (input);
	}
	if ( r_shownormals->integer ) {
		DrawNormals (input);
	}
	// clear shader so we can tell we don't have any unclosed surfaces
	tess.numIndexes = 0;
	tess.numVertexes = 0;
	tess.firstIndex = 0;
	tess.useCacheVao = qfalse;
	tess.useInternalVao = qfalse;

	GLimp_LogComment( "----------\n" );
}
