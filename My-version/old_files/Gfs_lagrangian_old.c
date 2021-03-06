#include "Gfs_lagrangian.h"

#ifdef HAVE_MPI
#include "mpi_boundary.h"
#endif /*HAVE_MPI*/
#ifdef HAVE_MPI

void mpi_particle_numbering(GfsDomain *domain, GfsLagrangianParticles *lagrangian)
{

  if(domain->pid < 0)
    return;

  guint comm_size;
  MPI_Comm_size(MPI_COMM_WORLD, &comm_size);
  guint idadd[comm_size], i;

  for(i=0; i < comm_size; i++){
    if(i == domain->pid)
      idadd[i] = lagrangian->idlast - lagrangian->maxid;
    else
      idadd[i] = 0;
    gfs_all_reduce(domain, idadd[i], MPI_INT, MPI_MAX);         //Not defined anymore
  }

  for(i = 1; i <= domain->pid; i++)
    idadd[i] += idadd[i-1];

  GSList *j = lagrangian->particles;
  while(j){
    Particle *p = (Particle *) (j->data);
    if(p->id > lagrangian->maxid)
      p->id = lagrangian->maxid + idadd[domain->pid]--;
    j = j->next;
  }
}

#endif /*HAVE_MPI*/



/*Subtraction of two vectors*/
static void subs_fttvectors (FttVector *a, FttVector *b, FttVector *result) {
    	
	result->x  = a->x - b->x;
    	result->y  = a->y - b->y;
	result->z  = a->z - b->z;
}

/*Same as in source.c used here to obtained viscosity*/
static GfsSourceDiffusion * source_diffusion_viscosity (GfsVariable * v)
{
  if (v->sources) {
    GSList * i = GTS_SLIST_CONTAINER (v->sources)->items;
    
    while (i) {
      GtsObject * o = i->data;

      if (GFS_IS_SOURCE_DIFFUSION (o))
        return GFS_SOURCE_DIFFUSION (o);
      i = i->next;
    } 
  }
  return NULL;
}


/*Calculation of vorticity vector*/
static void gfs_vorticity_vector (FttCell *cell, GfsVariable **v, FttVector *vort) {
 
	gdouble size;
 
        if (cell == NULL) return;
       	if (v == NULL) return;
 
        size = ftt_cell_size (cell);
 
        #if FTT_2D
 
        vort->x = 0.;
        vort->y = 0.;
        vort->z = (gfs_center_gradient (cell, FTT_X, v[1]->i) - gfs_center_gradient (cell, FTT_Y, v[0]->i))/size;
 
        #else /*FTT_3D*/
 
        vort->x = (gfs_center_gradient (cell, FTT_Y, v[2]->i) - gfs_center_gradient (cell, FTT_Z, v[1]->i))/size;
        vort->y = (gfs_center_gradient (cell, FTT_Z, v[0]->i) - gfs_center_gradient (cell, FTT_X, v[2]->i))/size;
        vort->z = (gfs_center_gradient (cell, FTT_X, v[1]->i) - gfs_center_gradient (cell, FTT_Y, v[0]->i))/size;
        #endif
 }
 
/*Various forces acting on the particle due to its motion in the fluid*/
typedef struct {
        Particle *p;
        GfsVariable **u, **un;
        FttVector *force;
        ForceCoefficients *fcoeffs;
        gdouble fluid_rho, viscosity, dt;
       	GfsLagrangianParticles *lagrangian;
} ForceParams;

typedef void (*force_pointer) (ForceParams * pars);

/*Calculation of the lift force acting on the unit volume of the particle*/
static void compute_lift_force(ForceParams * pars) {

        Particle *p = pars->p;
        GfsVariable **u = pars->u;
        gdouble fluid_rho = pars->fluid_rho;
        FttVector *force = pars->force;
        ForceCoefficients *fcoeffs = pars->fcoeffs;

        /*Interpolating fluid velocity to obtain the value at the position of the particle*/
        FttVector fluid_vel;
        fluid_vel.x = gfs_interpolate(p->cell, p->pos, u[0]);
        fluid_vel.y = gfs_interpolate(p->cell, p->pos, u[1]);
        #if !FTT_2D
                fluid_vel.z = gfs_interpolate(p->cell, p->pos, u[2]);
        #endif

        /*Subtrating fluid from particle velocity vectors to obtain the relative velocity */
        FttVector relative_vel;
        subs_fttvectors(&fluid_vel, &p->vel, &relative_vel);

        /*Calculation of vorticity */
        FttVector vorticity;
        gfs_vorticity_vector (p->cell, u, &vorticity);

        /*lift coefficient*/
        fcoeffs->cl = 0.5;

        /*lift force calculation*/
        #if FTT_2D
        force->x = fluid_rho*fcoeffs->cl*relative_vel.y*vorticity.z;
        force->y = -fluid_rho*fcoeffs->cl*relative_vel.x*vorticity.z;
        #else
        force->x = fluid_rho*fcoeffs->cl*(relative_vel.y*vorticity.z - relative_vel.z*vorticity.y);
        force->y = fluid_rho*fcoeffs->cl*(relative_vel.z*vorticity.x - relative_vel.x*vorticity.z);
        force->z = fluid_rho*fcoeffs->cl*(relative_vel.x*vorticity.y - relative_vel.y*vorticity.x);
        #endif

    return;
}

/*Calculation of drag force acting on the unit volume of the particle */
static void compute_drag_force (ForceParams *pars) {

        Particle *p = pars->p;
        GfsVariable **u = pars->u;
        gdouble fluid_rho = pars->fluid_rho;
        gdouble viscosity = pars->viscosity;
        ForceCoefficients *fcoeffs = pars->fcoeffs;
        FttVector *force = pars->force;

        /*Interpolating fluid velocity to obtain the value at the position of the particle*/
        FttVector fluid_vel;
        fluid_vel.x = gfs_interpolate(p->cell, p->pos, u[0]);
        fluid_vel.y = gfs_interpolate(p->cell, p->pos, u[1]);
        #if !FTT_2D
                fluid_vel.z = gfs_interpolate(p->cell, p->pos, u[2]);
        #endif

        /*Subtrating fluid from particle velocity vectors to obtain the relative velocity */
        FttVector relative_vel;
        subs_fttvectors(&fluid_vel, &p->vel, &relative_vel);

        gdouble radius;
	radius = pow(p->volume/M_PI, 1./2.);
        #if !FTT_2D
        radius = pow(3.0*(p->volume)/4.0/M_PI, 1./3.);
        #endif


        /* Calculation of the magnitude of the relative velocity*/
        #if !FTT_2D                     
        gdouble norm_relative_vel = sqrt(relative_vel.x*relative_vel.x +
                                        relative_vel.y*relative_vel.y +
                                        relative_vel.z*relative_vel.z);
        #else
        gdouble norm_relative_vel = sqrt(relative_vel.x*relative_vel.x +
                                        relative_vel.y*relative_vel.y);
        #endif


	/* Calculation of Reynolds number */
        gdouble Re;
        if(viscosity == 0){
                force->x = 0.;
                force->y = 0.;
                force->z = 0.;
        return;
        }
        else
        Re = 2.*norm_relative_vel*radius*fluid_rho/viscosity;




        if(fcoeffs->cdrag){

	///GFS_VARIABLE has only 1 arguments - I dont understand this
//        GFS_VARIABLE(p->cell, pars->lagrangian->reynolds->i) = Re;

//        GFS_VARIABLE(p->cell, pars->lagrangian->urel->i) = relative_vel.x;

//        GFS_VARIABLE(p->cell, pars->lagrangian->vrel->i) = relative_vel.y;

        #if !FTT_2D
//        GFS_VARIABLE(p->cell, pars->lagrangian->wrel->i) = relative_vel.z;
        #endif

//        GFS_VARIABLE(p->cell, pars->lagrangian->pdia->i) = 2.0*radius;

        fcoeffs->cd = gfs_function_value (fcoeffs->cdrag, p->cell);
        force->x = fcoeffs->cd*relative_vel.x*fluid_rho;
        force->y = fcoeffs->cd*relative_vel.y*fluid_rho;
        force->z = fcoeffs->cd*relative_vel.z*fluid_rho;

        return;
        }

	if(Re < 1e-8){
                force->x = 0.;
                force->y = 0.;
                force->z = 0.;
                return;
        }
        else if(Re < 50.0)
                fcoeffs->cd = 16.*(1. + 0.15*pow(Re,0.5))/Re;
        else
                fcoeffs->cd = 48.*(1. - 2.21/pow(Re,0.5))/Re;

        force->x = 3./(8.*radius)*fcoeffs->cd*norm_relative_vel*relative_vel.x*fluid_rho;
        force->y = 3./(8.*radius)*fcoeffs->cd*norm_relative_vel*relative_vel.y*fluid_rho;
        force->z = 3./(8.*radius)*fcoeffs->cd*norm_relative_vel*relative_vel.z*fluid_rho;
}


/*Calculation of the buoyant forces acting on the unit volume of the particle*/
static void compute_buoyant_force(ForceParams * pars) {

        Particle *p = pars->p;
        GfsVariable **u = pars->u;
        gdouble fluid_rho = pars->fluid_rho;
        FttVector * force = pars->force;
        gdouble g[3];
        FttComponent c;

	///Couldnt clearly understand this thing
        for(c = 0; c < FTT_DIMENSION; c++){
                g[c] = 0.;
                if (u[c]->sources) {
                        GSList * i = GTS_SLIST_CONTAINER (u[c]->sources)->items;

                while (i) {
                        if (GFS_IS_SOURCE (i->data)) {
                                g[c] += gfs_function_value (GFS_SOURCE ((GfsSourceGeneric *) i->data)->intensity,
                                                                p->cell);
                        }
                        i = i->next;
                }
                }
        }

	///p->density-fluid_rho is not defined
        force->x = (fluid_rho)*g[0];
        force->y = (fluid_rho)*g[1];
        #if !FTT_2D
        force->z = (fluid_rho)*g[2];
        #endif

}

/*Calculation of the inertial forces on the unit volume of the particle*/
static void compute_inertial_force(ForceParams * pars) {
        Particle *p = pars->p;
        GfsVariable **u = pars->u;
        gdouble fluid_rho = pars->fluid_rho;
        FttVector * force = pars->force;

        GfsVariable **un = pars->un;
        gdouble dt = pars->dt;

	//gdouble size = ftt_cell_size(p->cell);


        FttVector fluid_vel;
        FttVector fluid_veln;
        fluid_vel.x = gfs_interpolate(p->cell, p->pos, u[0]);
        fluid_vel.y = gfs_interpolate(p->cell, p->pos, u[1]);

        fluid_veln.x = gfs_interpolate(p->cell, p->pos, un[0]);
        fluid_veln.y = gfs_interpolate(p->cell, p->pos, un[1]);
        #if !FTT_2D
                fluid_vel.z = gfs_interpolate(p->cell, p->pos, u[2]);
                fluid_veln.z = gfs_interpolate(p->cell, p->pos, un[2]);
        #endif

	/*Calculation of local derivative*/
	if(dt > 0.){
                force->x = fluid_rho*(fluid_vel.x - fluid_veln.x)/dt;
                force->y = fluid_rho*(fluid_vel.y - fluid_veln.y)/dt;
        #if !FTT_2D
                force->z = fluid_rho*(fluid_vel.z - fluid_veln.z)/dt;
        #endif
        }

	///GFS_VARIABLE has only one argument
	///Gradient is already normalized by the size of the cell.
//	FttComponent c;
//        for(c=0;c < FTT_DIMENSION;c++){
//                force->x += fluid_rho*gfs_center_gradient(p->cell, c, u[0]->i)*
//                        GFS_VARIABLE(p->cell, u[c]->i)/size;
//                force->y += fluid_rho*gfs_center_gradient(p->cell, c, u[1]->i)*
//                        GFS_VARIABLE(p->cell, u[c]->i)/size;
//        #if !FTT_2D
//                force->z += fluid_rho*gfs_center_gradient(p->cell, c, u[2]->i)*
//                        GFS_VARIABLE(p->cell, u[c]->i)/size;
//        #endif

	/*Appending Convective derivative to local derivative to get the total force*/
        FttComponent c;
        for(c=0;c < FTT_DIMENSION;c++){
                force->x += fluid_rho*gfs_center_gradient(p->cell, c, u[0]->i)*GFS_VALUE(p->cell, u[c]);
                force->y += fluid_rho*gfs_center_gradient(p->cell, c, u[1]->i)*GFS_VALUE(p->cell, u[c]);
        #if !FTT_2D
                force->z += fluid_rho*gfs_center_gradient(p->cell, c, u[2]->i)*GFS_VALUE(p->cell, u[c]);
        #endif
   	}
}


/*Calculation of the added mass force acting on the unit volume of the particle*/  
static void compute_amf_force(ForceParams * pars) {

        Particle *p = pars->p;
        GfsVariable **u = pars->u;
        gdouble fluid_rho = pars->fluid_rho;
        FttVector * force = pars->force;
        ForceCoefficients * fcoeffs = pars->fcoeffs;

        GfsVariable **un = pars->un;
        gdouble dt = pars->dt;

        //gdouble size = ftt_cell_size(p->cell);

        FttVector fluid_vel;
        FttVector fluid_veln;
        fluid_vel.x = gfs_interpolate(p->cell, p->pos, u[0]);
        fluid_vel.y = gfs_interpolate(p->cell, p->pos, u[1]);

        fluid_veln.x = gfs_interpolate(p->cell, p->pos, un[0]);
        fluid_veln.y = gfs_interpolate(p->cell, p->pos, un[1]);
        #if !FTT_2D
                fluid_vel.z = gfs_interpolate(p->cell, p->pos, u[2]);
                fluid_veln.z = gfs_interpolate(p->cell, p->pos, un[2]);
        #endif

	/*Calculation of local derivative*/
	if(dt > 0.){
                force->x = fluid_rho*(fluid_vel.x - fluid_veln.x)/dt;
                force->y = fluid_rho*(fluid_vel.y - fluid_veln.y)/dt;
        #if !FTT_2D
                force->z = fluid_rho*(fluid_vel.z - fluid_veln.z)/dt;
        #endif
        }

	///GFS_VARIABLE takes only 1 argument
	///gfs_center_gradient is normalized by cell size
//        FttComponent c;
//        for(c = 0; c < FTT_DIMENSION; c++){
//                force->x += fluid_rho*gfs_center_gradient(p->cell, c, u[0]->i)*
//                        GFS_VARIABLE(p->cell, u[c]->i)/size;
//                force->y += fluid_rho*gfs_center_gradient(p->cell, c, u[1]->i)*
//                        GFS_VARIABLE(p->cell, u[c]->i)/size;
//        #if !FTT_2D
//                force->z += fluid_rho*gfs_center_gradient(p->cell, c, u[2]->i)*
//                        GFS_VARIABLE(p->cell, u[c]->i)/size;
//        #endif


	/*Appending Convective derivative to local derivative to get the total force*/
        FttComponent c;
        for(c=0;c < FTT_DIMENSION;c++){
                force->x += fluid_rho*gfs_center_gradient(p->cell, c, u[0]->i)*GFS_VALUE(p->cell, u[c]);
                force->y += fluid_rho*gfs_center_gradient(p->cell, c, u[1]->i)*GFS_VALUE(p->cell, u[c]);
        #if !FTT_2D
                force->z += fluid_rho*gfs_center_gradient(p->cell, c, u[2]->i)*GFS_VALUE(p->cell, u[c]);
        #endif

   }

	///Particle velocity subtraction is missing - done later while calculating the particle acceleration 
//	force->x -= fluid_rho*p->acc.x;
//	force->y -= fluid_rho*p->acc.y;
//	force->z -= fluid_rho*p->acc.z; 

	//Coefficient
        force->x = fcoeffs->cm*force->x;
        force->y = fcoeffs->cm*force->y;
        force->z = fcoeffs->cm*force->z;
}


static void compute_faxen_force(ForceParams * pars) {
  pars->force->x = 0.;
  pars->force->y = 0.;
  pars->force->z = 0.;
}

static void compute_basset_force(ForceParams * pars) {
  pars->force->x = 0.;
  pars->force->y = 0.;
  pars->force->z = 0.;
}

/* Summation of the forces acting on the particle*/ //Only inertial and buoyancy forces are added
static void particles_force(ForceParams * pars) {
        Particle *p = pars->p;
        FttVector * force = pars->force;
        gdouble fluid_rho = pars->fluid_rho;
        ForceCoefficients * fcoeffs = pars->fcoeffs;

        if(p->cell==NULL)
                return;


        gdouble p3dvolume = p->volume;

	/*LHS of Momentum equation of the particle*/
        p->phiforce.x = -p->acc.x*(p->density)* p3dvolume;
        p->phiforce.y = -p->acc.y*(p->density)* p3dvolume;
        p->phiforce.z = -p->acc.z*(p->density)* p3dvolume;

	/*Buoyancy force added to RHS of the momentum equation of the particle*/
        if(fcoeffs->buoy == 1){
                compute_buoyant_force(pars);
                p->phiforce.x += ((force->x)* p3dvolume);
                p->phiforce.y += ((force->y)* p3dvolume);
                p->phiforce.z += ((force->z)* p3dvolume);
        }

	/*Inertial force added to RHS of the momentum equation of the particle*/
        if(fcoeffs->inertial == 1){
                compute_inertial_force(pars);
                p->phiforce.x += ((force->x)* p3dvolume);
                p->phiforce.y += ((force->y)* p3dvolume);
                p->phiforce.z += ((force->z)* p3dvolume);
        }

        //recheck - fluid density is being divided 
        p->phiforce.x /=(fluid_rho);
        p->phiforce.y /=(fluid_rho);
        p->phiforce.z /=(fluid_rho);

}

/*Create force variable for the domain*/
static void couple_force_define(GfsDomain * d, GfsVariable **force, GString * s) {

 	force[0] = gfs_domain_get_or_add_variable(d, g_strconcat(s->str, "x", NULL),
                                            "x-component of the Lagrangian Particle Force");
  	force[1] = gfs_domain_get_or_add_variable(d, g_strconcat(s->str, "y", NULL),
                                            "y-component of the Lagrangian Particle Force");
	#if !FTT_2D
  	force[2] = gfs_domain_get_or_add_variable(d, g_strconcat(s->str, "z", NULL),
                                            "z-component of the Lagrangian Particle Force");
	#endif
}


static void couple_force_init(FttCell *c, GfsVariable * f)
{
 // GFS_VARIABLE(c, f->i) = 0.;
  	GFS_VALUE(c, f) = 0.;
	
}

static void reset_couple_force (GfsDomain *domain, GfsVariable **f)
{
  FttComponent c;
  FttDirection d;
  for(c = 0; c < FTT_DIMENSION; c++){
    gfs_domain_cell_traverse (domain, FTT_PRE_ORDER, FTT_TRAVERSE_ALL, -1,
                              (FttCellTraverseFunc)couple_force_init,
                              f[c]);
  }
}


/*Smoothening the force using Gaussian smoothing Exp(-x^2/sigma^2)*/
static void compute_smooth_force(FttCell *cell, gpointer * data) {

        if(!cell) return;

        Particle *p = (Particle *)(data[0]);
        GfsVariable **f = data[1];
        gdouble sigma = *(gdouble *)data[2];
        FttVector pos;
        ftt_cell_pos(cell, &pos);

	//distance of the cell being considered from the particle being considered
        gdouble dist = (p->pos.x - pos.x)*(p->pos.x - pos.x)
                        + (p->pos.y - pos.y)*(p->pos.y - pos.y);

        #if !FTT_2D
                dist += (p->pos.z - pos.z)*(p->pos.z - pos.z);
        #endif
                dist = exp(-dist/(sigma*sigma))/(2.*M_PI*sigma*sigma);
        #if !FTT_2D
                dist /= (pow(2.*M_PI,0.5)*sigma);
        #endif
                dist /= (pow(2.*M_PI,0.5)*sigma);

	///GFS_VARIABLE has only 1 argument
//        GFS_VARIABLE(cell, f[0]->i) += p->phiforce.x*dist;
//        GFS_VARIABLE(cell, f[1]->i) += p->phiforce.y*dist;
//        #if !FTT_2D
//                GFS_VARIABLE(cell, f[2]->i) += p->phiforce.z*dist;
//        #endif

        GFS_VALUE(cell, f[0]) += p->phiforce.x*dist;
        GFS_VALUE(cell, f[1]) += p->phiforce.y*dist;
        #if !FTT_2D
                GFS_VALUE(cell, f[2]) += p->phiforce.z*dist;
        #endif
	
}

static gboolean check_stencil(FttCell * cell, FttVector pos0, gdouble sigma, FttDirection *d)
{
  if(!cell) return FALSE;

  gdouble size = ftt_cell_size(cell);
  FttVector pos1;
  ftt_cell_pos(cell, &pos1);

  FttDirection d0;
  gdouble dist, dist1;
  gboolean check = TRUE;
  for(d0 = 0; d0 < FTT_DIMENSION; d0++){
    dist = abs(((&pos1.x)[d0] - size/2.0 - (&pos0.x)[d0]));
    dist1 = abs(((&pos1.x)[d0] + size/2.0 - (&pos0.x)[d0]));

    if(dist < dist1)
      d[d0] = 2.0*d0;
    else
      d[d0] = 2.0*d0 + 1;

    if(dist > sigma || dist1 > sigma)
      check = FALSE;
  }

  return check;
}

/*Gaussian Smoothed two-way coupling force applied on all cells in the domain is not included here*/

/*Gaussian unsmoothed two-way coupling force is not included*/

/*Gaussian Smoothed Two-way Coupling Force: Applied only on 3Sigma surroundings*/
static void compute_coupling_force (Particle *p, GfsVariable **f)
{
     	if(!p->cell) return;
    
     	gpointer data[3];
      	data[0] = p;
      	data[1] = f;
    
      	gdouble size = ftt_cell_size(p->cell);
      	gdouble radius = pow(p->volume/M_PI,1./2.);
    	#if !FTT_2D
      		radius = pow(3.0*(p->volume)/4.0/M_PI, 1./3.);
    	#endif
    
      	gdouble sigma = MAX(2.*radius, size)/2.;
      	data[2] = &sigma;
    
      	FttCell *cell = p->cell, *neighbor1, *neighbor2;
      	FttDirection d[FTT_DIMENSION];
      	while(!FTT_CELL_IS_ROOT(cell) && ftt_cell_parent(cell)
            && check_stencil(cell, p->pos, 3.*sigma, d)){
        cell = ftt_cell_parent(cell);
      	}
    
    	/*   if(FTT_CELL_IS_ROOT(cell)) */
    	/*     return; */
    
      	g_assert(cell!=NULL);
    
      	ftt_cell_traverse (cell, FTT_PRE_ORDER, FTT_TRAVERSE_LEAFS, -1,
                         (FttCellTraverseFunc) compute_smooth_force, data);
    
      	FttDirection d0, d1;
      	for(d0 = 0; d0 < FTT_DIMENSION; d0++){
        	neighbor1 = ftt_cell_neighbor(cell, d[d0]);
        	if(neighbor1)
          	ftt_cell_traverse (neighbor1, FTT_PRE_ORDER, FTT_TRAVERSE_LEAFS, -1,
                             (FttCellTraverseFunc) compute_smooth_force, data);
        	else
          	continue;
      	/*   Do for neighbor if it exists else continue */
        for(d1 = d0 + 1; d1 < FTT_DIMENSION; d1++){
          	neighbor2 = ftt_cell_neighbor(neighbor1, d[d1]);
       		if(neighbor2){
            	ftt_cell_traverse (neighbor2, FTT_PRE_ORDER, FTT_TRAVERSE_LEAFS, -1,
                               (FttCellTraverseFunc) compute_smooth_force, data);
     	}
       		/*    Do for neighbor if it exists else continue */
        }
      	}
    
    	#if !FTT_2D
      	/*Corner Cell*/
      	neighbor1 = ftt_cell_neighbor(cell, d[0]);
      	if(neighbor1)
        	neighbor1 = ftt_cell_neighbor(neighbor1, d[1]);
      	if(neighbor1)
        	neighbor1 = ftt_cell_neighbor(neighbor1, d[2]);
      	if(neighbor1)
        	ftt_cell_traverse (neighbor1, FTT_PRE_ORDER, FTT_TRAVERSE_LEAFS, -1,
                           (FttCellTraverseFunc) compute_smooth_force, data);
    	#endif
    
}




/*Updating the particle acceleration*/
static void compute_particle_acceleration(Particle *p, FttVector force, gdouble cm, gdouble fluid_rho) {

	//Taking a component of the added mass force to the LHS of the momentum equation of the particle
	p->acc.x += force.x/(p->density + fluid_rho*cm);
	p->acc.y += force.y/(p->density + fluid_rho*cm);
	#if !FTT_2D
  		p->acc.z += force.z/(p->density + fluid_rho*cm);
	#endif
}

/*Updating the particle velocity due to forces acting on the particle*/
static void compute_particle_velocity (Particle * p, double dt) {
	
	p->vel.x +=  dt* p->acc.x;
  	p->vel.y +=  dt* p->acc.y;
	#if !FTT_2D
  		p->vel.z +=  dt* p->acc.z;
	#endif
}

/*Updating the particle position*/
static void advect_particle (Particle * p, double dt) {

	p->pos.x +=  dt* p->vel.x;
  	p->pos.y +=  dt* p->vel.y;
	#if !FTT_2D
  		p->pos.z +=  dt* p->vel.z;
	#endif
}

/*Updating the particle velocity due to fluid velocity*/
static void fluidadvect_particles(Particle * p, GfsVariable **u) {

	if(p->cell!=NULL) {
		p->vel.x = gfs_interpolate(p->cell, p->pos, u[0]);
    		p->vel.y = gfs_interpolate(p->cell, p->pos, u[1]);
		#if !FTT_2D
   	 		p->vel.z = gfs_interpolate(p->cell, p->pos, u[2]);
		#endif    
  	}
}

/* Initializes particle velocity*/
static void init_particles(GfsLagrangianParticles * l, GfsDomain *domain) {

  	GSList *i = l->particles;
  	Particle *p;
  	GfsVariable ** u = gfs_domain_velocity (domain);
  	
	while(i) {
    	p = (Particle *) i->data;

	///gfs_domain_locate takes 4 arguments
	//p->cell = gfs_domain_locate(domain, p->pos, -1);
    	p->cell = gfs_domain_locate(domain, p->pos, -1, NULL);

    	if(p->cell!=NULL){
      		p->vel.x = gfs_interpolate(p->cell, p->pos, u[0]);
      		p->vel.y = gfs_interpolate(p->cell, p->pos, u[1]);
	#if !FTT_2D
      		p->vel.z = gfs_interpolate(p->cell, p->pos, u[2]);
	#endif
    	}
    	i = i->next;
  	}
}

/*Copying value for previous time*/
static void copy_cell_gfsvariable(FttCell *c, gpointer * data) {
  	GfsVariable * v1 = (GfsVariable *) data[0];
  	GfsVariable * v2 = (GfsVariable *) data[1];

	///GFS_VARIABLE no longer correct	
	//GFS_VARIABLE(c, v1->i) = GFS_VARIABLE(c, v2->i);
	GFS_VALUE(c, v1) = GFS_VALUE(c, v2);
	
}


/*Storing previous time step value*/
static void store_domain_previous_vel(GfsDomain *d, GfsVariable **un)
{
  GfsVariable ** u = gfs_domain_velocity (d);
  FttComponent c;
  for(c = 0; c < FTT_DIMENSION; c++){
    gpointer data[2];

    data[0] = un[c];
    data[1] = u[c];
    gfs_domain_cell_traverse (d, FTT_PRE_ORDER, FTT_TRAVERSE_LEAFS, -1,
                              (FttCellTraverseFunc)copy_cell_gfsvariable,
                              data);
  }
}


static void previous_time_vel(GfsDomain *d, GfsVariable **un) {

  un[0] = gfs_domain_get_or_add_variable(d, "Un", "x-component of Velocity at previous time step");
  un[1] = gfs_domain_get_or_add_variable(d, "Vn", "y-component of Velocity at previous time step");
#if !FTT_2D
  un[2] = gfs_domain_get_or_add_variable(d, "Wn", "z-component of Velocity at previous time step");
#endif
}

/*Particle Boundary Conditions*/
/* 1)Identify the boundary through which particle passes
   2)Identify the velocity bcs employed at the box boundary
   3)Identify the pid of the current box in which the particle lies
   4)Send the particle Info to the pid with Tag PARTICLE
   5)Receive the particle Info and append it to the corresponding Particle list
   6)Remove the link and free the particle
*/

typedef struct {
  gint boxid;
  FttDirection d;
  Particle *p;
} Particle_send;

/*Checks the intersection of a particle path ray(a line segment) with the boundaries of the containee cell*/
static gboolean check_intersetion(FttVector cellpos, FttVector p0, FttVector p1,
                                  FttDirection *dstore, gdouble size)
{

  gdouble t;
  FttDirection d;


  for(d = 0; d < FTT_NEIGHBORS; d++){

    gdouble normal = ((gdouble) FTT_OPPOSITE_DIRECTION(d) - (gdouble)d);

#if FTT_2D
    switch(d/2){
    case 0:
      if((p1.x - p0.x)!=0 && normal*(p1.x-p0.x) > 0){
        t = (cellpos.x + normal*size*0.5 - p0.x)/(p1.x - p0.x);
        gdouble py = p0.y + t*(p1.y - p0.y);
        if((py - cellpos.y + size*0.5)*(py - cellpos.y - size*0.5) <= 0  &&  t*(t-1)<= 0 ){
          *dstore = d;
          return TRUE;
        }
      }
      break;
    case 1:
      if((p1.y - p0.y)!=0 && normal*(p1.y-p0.y) > 0){
        t = (cellpos.y + normal*size*0.5- p0.y)/(p1.y - p0.y);

        gdouble px = p0.x + t*(p1.x - p0.x);
        if((px - cellpos.x + size*0.5)*(px - cellpos.x - size*0.5) <= 0  &&  t*(t-1) <= 0){
          *dstore = d;
          return TRUE;
        }
      }
      break;
    default: g_assert_not_reached();
    }
#else
    switch(d/2){
    case 0:
      if((p1.x - p0.x)!=0 && normal*(p1.x-p0.x) > 0){
        t = (cellpos.x + normal*size*0.5 - p0.x)/(p1.x - p0.x);
        gdouble py = p0.y + t*(p1.y - p0.y);
        gdouble pz = p0.z + t*(p1.z - p0.z);
        if((py - cellpos.y + size*0.5)*(py - cellpos.y - size*0.5) <= 0  &&
           (pz - cellpos.z + size*0.5)*(pz - cellpos.z - size*0.5) <= 0
           &&  t*(t-1)<= 0 )
          {
          *dstore = d;
          return TRUE;
        }
      }
      break;
    case 1:
      if((p1.y - p0.y)!=0 && normal*(p1.y-p0.y) > 0){
        t = (cellpos.y + normal*size*0.5- p0.y)/(p1.y - p0.y);

        gdouble px = p0.x + t*(p1.x - p0.x);
        gdouble pz = p0.z + t*(p1.z - p0.z);
        if((px - cellpos.x + size*0.5)*(px - cellpos.x - size*0.5) <= 0  &&
           (pz - cellpos.z + size*0.5)*(pz - cellpos.z - size*0.5) <= 0
           &&  t*(t-1)<= 0 )
          {
          *dstore = d;
          return TRUE;
        }
      }
      break;
    case 2:
      if((p1.z - p0.z)!=0 && normal*(p1.z-p0.z) > 0){
        t = (cellpos.z + normal*size*0.5- p0.z)/(p1.z - p0.z);

        gdouble px = p0.x + t*(p1.x - p0.x);
        gdouble py = p0.y + t*(p1.y - p0.y);
        if((px - cellpos.x + size*0.5)*(px - cellpos.x - size*0.5) <= 0  &&
           (py - cellpos.y + size*0.5)*(py - cellpos.y - size*0.5) <= 0
           &&  t*(t-1)<= 0 )
          {
          *dstore = d;
          return TRUE;
        }
      }
      break;
    default: g_assert_not_reached();
    }
#endif
  }

  g_warning("Intersection finding algorithm failed\n");
  return FALSE;
}




/* Locates the cell in a domain through which the particle path-ray passes in case both the end points of 
   the ray do not lie in the domain */
static FttCell * locate_particle_ray_cell(GfsDomain *domain, FttVector pos0, FttVector pos1)
{

  FttCell *cell;
  FttVector pos;
  gdouble c = 0;

  //cell = gfs_domain_locate(domain, pos0, -1);
  cell = gfs_domain_locate(domain, pos0, -1, NULL);
  //if(cell && cell == gfs_domain_locate(domain, pos1, -1))
  if(cell && cell == gfs_domain_locate(domain, pos1, -1, NULL))
    return cell;

  while(c < 1){
    c = c + 1/100.;
    pos.x = (1-c)*pos0.x + c*pos1.x;
    pos.y = (1-c)*pos0.y + c*pos1.y;
    pos.z = (1-c)*pos0.z + c*pos1.z;
    //cell = gfs_domain_locate(domain, pos, -1);
    cell = gfs_domain_locate(domain, pos, -1, NULL);

    if(cell)
      return cell;

    pos.x = c*pos0.x + (1-c)*pos1.x;
    pos.y = c*pos0.y + (1-c)*pos1.y;
    pos.z = c*pos0.z + (1-c)*pos1.z;
    //cell = gfs_domain_locate(domain, pos, -1);
    cell = gfs_domain_locate(domain, pos, -1, NULL);
    if(cell)
      return cell;
  }

  printf("pos0: x:%lf y:%lf, pos1:x:%lf y:%lf\n",pos0.x,pos0.y,pos1.x,pos1.y);
  g_warning("Particle Ray could not be tracked in the domain\n");
  return NULL;
}

/*Solid reflection is not included here*/

/*Tracks the particle path ray to identify boundary cell for the application of the Boundary Conditions*/
static FttCell * boundarycell ( GfsDomain *domain, Particle *p, FttDirection *dstore, gdouble dt)
{
  FttCell *cell;
  FttVector p0, cellpos;
  gdouble size;

  advect_particle(p, -dt);
  //cell = gfs_domain_locate(domain, p->pos, -1);
  cell = gfs_domain_locate(domain, p->pos, -1, NULL);
  p0 = p->pos;
  advect_particle(p, dt);

  if(!cell)
    cell = locate_particle_ray_cell(domain, p0, p->pos);


  if(!cell)
    return NULL;

  g_assert(cell!=NULL);
  ftt_cell_pos(cell, &cellpos);
  size = ftt_cell_size(cell);
  check_intersetion(cellpos, p0, p->pos, dstore, size);
  FttCellFace face = ftt_cell_face(cell, *dstore);

  if(!face.neighbor)
    return cell;

  while(!GFS_CELL_IS_BOUNDARY(face.neighbor)) {
    cell = face.neighbor;
    g_assert(cell!=NULL);
    ftt_cell_pos(cell, &cellpos);
    size = ftt_cell_size(cell);
    check_intersetion(cellpos, p0, p->pos, dstore, size);
    face = ftt_cell_face(cell, *dstore);

    if(!face.neighbor)
      return cell;
  };

  if(cell)
    return cell;
  else
    g_assert_not_reached();

}

/*Reflection Boundary Condition*/
static void reflection_bc_particle(FttDirection d, GfsBox * box, GSList *packet_send,
                                   GfsLagrangianParticles *lagrangian)
{
  FttVector box_face;
  g_assert(box->root!=NULL);
  ftt_cell_pos(box->root, &box_face);
  gdouble size = ftt_cell_size(box->root);

  gint normal = FTT_OPPOSITE_DIRECTION(d) - d;
  GSList *i = packet_send;

  FttComponent c = d/2;
  (&box_face.x)[c] += (gdouble)normal * size/2.;
  while(i){
    Particle *p1 = (Particle *) (i->data);
    gdouble distance = ((&p1->pos.x)[c] - (&box_face.x)[c])*normal;
    (&p1->pos.x)[c] = (&p1->pos.x)[c] - 2.*distance*normal;
    (&p1->vel.x)[c] = -(&p1->vel.x)[c];
    lagrangian->particles = g_slist_append(lagrangian->particles, p1);
    lagrangian->n++;
    i = i->next;
  }
}


/*Periodic boundary conditions*/
static void periodic_bc_particle(FttDirection d, GfsBox * box, GSList *packet_send,
                                 GfsLagrangianParticles *lagrangian)
{
  FttVector box_face, box_face_nbr;
  g_assert(box->root!=NULL && GFS_BOX(box->neighbor[d])->root!=NULL);
  ftt_cell_pos(box->root, &box_face);
  ftt_cell_pos(GFS_BOX(box->neighbor[d])->root, &box_face_nbr);
  gdouble size = ftt_cell_size(box->root);
  gdouble size_nbr = ftt_cell_size(GFS_BOX(box->neighbor[d])->root);

  gdouble normal = (gdouble)FTT_OPPOSITE_DIRECTION(d) - (gdouble) d;
  GSList *i = packet_send;

  (&box_face.x)[d/2] += (gdouble)normal * size/2.;
  (&box_face_nbr.x)[d/2] -= (gdouble)normal * size/2.;

  while(i){
      Particle *p1 = (Particle *) (i->data);
      gdouble distance = ((&p1->pos.x)[d/2] - (&box_face.x)[d/2])*normal;
      (&p1->pos.x)[d/2] = (&box_face_nbr.x)[d/2] + distance;
      lagrangian->particles = g_slist_append(lagrangian->particles, p1);
      lagrangian->n++;
    i = i->next;
  }

}


#ifdef HAVE_MPI
/*MPI Boundary conditions*/
typedef struct {
  GfsBoundaryMpi *mpi;
  FttDirection d;
  int nsends;
  GSList *packet_send;
} Packet_mpi_send;

typedef struct {
  GfsBoundaryMpi *mpi;
  FttDirection d;
  int nrcvs;
} Packet_mpi_rcv;

#define TAGs(boundary)           ((boundary)->d + (boundary)->box->id)
#define TAGr(boundary)  (FTT_OPPOSITE_DIRECTION ((boundary)->d) +\
                                 GFS_BOUNDARY_MPI (boundary)->id)

static void mpi_send_particle( Packet_mpi_send * packet, GfsLagrangianParticles *lagrangian)
{

  GfsBoundaryMpi * mpi = packet->mpi;
  FttDirection d = packet->d;
  FttDirection od = FTT_OPPOSITE_DIRECTION(d);
  GSList *packet_send = packet->packet_send;

  int nsends =  packet->nsends;

  MPI_Status status;
  MPI_Request request;
  int tags = 10000 + TAGs(GFS_BOUNDARY(mpi));
  int j;

  GfsDomain *domain = GFS_DOMAIN(gfs_object_simulation(lagrangian));

 /*  MPI_Send(&nsends, 1, MPI_INT, mpi->process, tags, MPI_COMM_WORLD); */
  MPI_Isend(&nsends, 1, MPI_INT, mpi->process, tags, MPI_COMM_WORLD,&request);
/*   MPI_Wait(&request, &status); */
  GSList *i = packet_send;

  gdouble packetS[9*nsends];

  gint k = 0;
  for(j = 0; j < nsends; j++){

    Particle *p1 = (Particle *) (i->data);

    packetS[0+k] =  p1->id;
    packetS[1+k] =  p1->pos.x;
    packetS[2+k] =  p1->pos.y;
    packetS[3+k] =  p1->pos.z;
    packetS[4+k] =  p1->vel.x;
    packetS[5+k] =  p1->vel.y;
    packetS[6+k] =  p1->vel.z;
    packetS[7+k] =  p1->volume;
    packetS[8+k] =  p1->density;

    k = (j+1)*9;
    i = i->next;
    packet_send = g_slist_remove(packet_send, p1);
    g_free(p1);
  }

  if(nsends > 0){
    MPI_Isend(packetS, 9*nsends, MPI_DOUBLE, mpi->process, tags, MPI_COMM_WORLD,&request);
    MPI_Wait(&request, &status);
  }

}

static void mpi_rcv_particle(Packet_mpi_rcv * packet, GfsLagrangianParticles *lagrangian)
{

  GfsBoundaryMpi * mpi = packet->mpi;
  FttDirection d = packet->d;
  int nrcvs;
  int tagr =  10000 + TAGr(GFS_BOUNDARY(mpi));
  MPI_Status status;
  MPI_Request request;

/*   MPI_Recv(&nrcvs, 1, MPI_INT, mpi->process, tagr, MPI_COMM_WORLD, &status); */
  MPI_Irecv(&nrcvs, 1, MPI_INT, mpi->process, tagr, MPI_COMM_WORLD, &request);
  MPI_Wait(&request, &status);

  gdouble packetR[9*nrcvs];
  if(nrcvs > 0){
    MPI_Recv(packetR, 9*nrcvs,  MPI_DOUBLE, mpi->process, tagr, MPI_COMM_WORLD, &status);
/*    MPI_Irecv(packetR, 9*nrcvs, MPI_DOUBLE, mpi->process, tagr, MPI_COMM_WORLD,&request); */
/*     MPI_Wait(&request, &status); */
  }
  else
    return;

  gint j;
  gint k = 0;
  for(j = 0; j < nrcvs; j++){
    Particle *p1 = g_malloc(sizeof(Particle));
    p1->id = (gint) packetR[0+k];
    p1->pos.x =  packetR[1+k];
    p1->pos.y =  packetR[2+k];
    p1->pos.z =  packetR[3+k];
    p1->vel.x =  packetR[4+k];
    p1->vel.y =  packetR[5+k];
    p1->vel.z =  packetR[6+k];
    p1->volume = packetR[7+k];
    p1->density = packetR[8+k];
    k = (j+1)*9;
    lagrangian->particles = g_slist_append(lagrangian->particles, p1);
    lagrangian->n++;
  }
}

#endif /*HAVE_MPI*/




static void send_particles(FttDirection d, GfsBoundary *b, GfsBox *box, gint nsends,
                           GSList *packet_send, GfsLagrangianParticles *lagrangian)
{

#ifdef HAVE_MPI
  GfsBoundaryMpi *mpi = GFS_BOUNDARY_MPI (b);
  if(GFS_IS_BOUNDARY_MPI(mpi)){
    /*MPI BC*/
    Packet_mpi_send *packet = g_malloc0(sizeof(Packet_mpi_send));
    packet->mpi = mpi;
    packet->d = d;
    packet->packet_send = packet_send;

    packet-> nsends = nsends;
    mpi_send_particle(packet, lagrangian);
    g_free(packet);
    return;
  }
#endif /*HAVE_MPI*/
  if(nsends > 0){
    GfsVariable **u = gfs_domain_velocity(gfs_box_domain(box));
    GfsBc * vel_bc = gfs_boundary_lookup_bc(b, u[d/2]);
    if(GFS_IS_BC_DIRICHLET(vel_bc)){
      /*Reflection of the particle*/
      reflection_bc_particle(d, box, packet_send, lagrangian);
    }
    else if(box->neighbor[d] && GFS_IS_BOUNDARY_PERIODIC(box->neighbor[d])){
      /*Periodic BC*/
      periodic_bc_particle(d, box, packet_send, lagrangian);
    }
    else{
      /*Remove the Particle*/
      GSList *i = packet_send;
      while(i){
        Particle *p1 = (Particle *) (i->data);
        i = i->next;
        packet_send = g_slist_remove(packet_send, p1);
        g_free(p1);
      }
    }
  }
}




static void rcv_particles(FttDirection d, GfsBoundary *b, GfsBox *box, gint nrcvs
                          , GfsLagrangianParticles *lagrangian )
{
#ifdef HAVE_MPI
  GfsBoundaryMpi *mpi = GFS_BOUNDARY_MPI (b);
  if(GFS_IS_BOUNDARY_MPI(mpi)){
    /*MPI BC*/
    Packet_mpi_rcv *packet = g_malloc0(sizeof(Packet_mpi_rcv));
    packet->mpi = mpi;
    packet->d = FTT_OPPOSITE_DIRECTION(d);
    packet->nrcvs  =   nrcvs;

    mpi_rcv_particle(packet, lagrangian);
    g_free(packet);
    return;
  }
#endif /*HAVE_MPI*/
}




/*Traverses boxes in the domain for application of boundary conditions on particles*/
static void box_send_bc(GfsBox *box, gpointer * datum)
{

  GSList *p_sends =  (GSList *) datum[0];
  GfsLagrangianParticles *lagrangian = (GfsLagrangianParticles *) datum[1];

  gint nsends[FTT_NEIGHBORS], nrcvs[FTT_NEIGHBORS];
  GSList *packet_send[FTT_NEIGHBORS];
  FttDirection d;

  for (d = 0; d < FTT_NEIGHBORS; d++){
    nsends[d] = 0;
    nrcvs[d] = 0;
    packet_send[d] = NULL;
  }

  GSList * i = p_sends;

  while(i){
    Particle_send *psend = (Particle_send *) (i->data);
    if(psend->boxid == box->id){
      nsends[psend->d]++;
      Particle *p1 = g_malloc(sizeof(Particle));
      memcpy (p1, psend->p, sizeof(Particle));
      packet_send[psend->d] = g_slist_append(packet_send[psend->d], psend->p);
    }
    i = i->next;
  }

  for (d = 0; d < FTT_NEIGHBORS; d++){
    /*If neighboring box does not exist -> slip-wall (Reflect)*/
    if(!box->neighbor[d]){
      if(nsends[d] > 0){
        reflection_bc_particle(d, box, packet_send[d], lagrangian);
      }
    }
    else if(GFS_IS_BOUNDARY(box->neighbor[d])){

      GfsBoundary *b = GFS_BOUNDARY(box->neighbor[d]);

      send_particles(d, b, box, nsends[d], packet_send[d], lagrangian);
    }
  }
}



static void box_rcv_bc(GfsBox *box, gpointer * datum)
{
  GSList *p_sends =  (GSList *) datum[0];
  GfsLagrangianParticles *lagrangian = (GfsLagrangianParticles *) datum[1];

  gint nsends[FTT_NEIGHBORS], nrcvs[FTT_NEIGHBORS];
  FttDirection d;

  for (d = 0; d < FTT_NEIGHBORS; d++){
    nrcvs[d] = 0;
    if(box->neighbor[d] && GFS_IS_BOUNDARY(box->neighbor[d])){
      GfsBoundary *b = GFS_BOUNDARY(box->neighbor[d]);
      rcv_particles(d, b, box, nrcvs[d], lagrangian);
    }
  }
}





static void boundary_particles(GfsLagrangianParticles *lagrangian, GfsDomain *domain)
{
  GSList * boundaryparticles = NULL;
  gdouble dt  = GFS_SIMULATION(domain)->advection_params.dt;
  GSList *nsends = NULL;

  /*Make a list of particles not found in the domain*/
  GSList *i = lagrangian->particles;
  while(i){
    Particle *p = (Particle *)(i->data);
    //p->cell = gfs_domain_locate(domain, p->pos, -1);
    p->cell = gfs_domain_locate(domain, p->pos, -1, NULL);

    if(!p->cell)
      boundaryparticles = g_slist_append(boundaryparticles, p);
    i = i->next;
  }

  /*Run over the Boundary Particles (Not found in the Domain) to Identify the box and the boundary and m     ake a nsend list of particles for each boundary*/
  i = boundaryparticles;
  while(i){

    Particle *p = (Particle *)(i->data);

    FttDirection dstore;

 /*To identify the boundary cell in situations where particle moves more than one cell in 1 time step. d     store holds the direction of the boundary from the cell*/
    FttCell *cell = boundarycell (domain, p, &dstore, dt);

    if(cell){

      while(!FTT_CELL_IS_ROOT(cell))
        cell = ftt_cell_parent (cell);

      GfsBox *box = GFS_BOX(FTT_ROOT_CELL(cell)->parent);

      if(dstore < 0 || dstore > FTT_NEIGHBORS)
        g_assert_not_reached();

      if(!box->neighbor[dstore] || GFS_IS_BOUNDARY(box->neighbor[dstore])){

        Particle_send *nsend = g_malloc(sizeof(Particle_send));

        nsend->boxid = box->id;
        nsend->d = dstore;
        nsend->p = p;

        nsends = g_slist_append(nsends, nsend);
      }
    }

    i = i->next;
    lagrangian->particles = g_slist_remove(lagrangian->particles, p);
  }

  /*Apply Box-wise BC*/
  gpointer datum[2];
  datum[0] = nsends;
  datum[1] = lagrangian;
  gts_container_foreach (GTS_CONTAINER (domain),
                         (GtsFunc) box_send_bc, datum);
  gts_container_foreach (GTS_CONTAINER (domain),
                         (GtsFunc) box_rcv_bc, datum);

  i = nsends;
  while(i){
    Particle_send *psend = (Particle_send *) (i->data);
    i = i->next;
    boundaryparticles =  g_slist_remove(boundaryparticles, psend->p);
    nsends = g_slist_remove(nsends, psend);
    g_free(psend);
  }
  g_slist_free(nsends);
  g_slist_free(boundaryparticles);
}


/*Immersed boundary method is not included*/

/*Collision not included*/


/*GfsLagrangianParticle event method*/
static gboolean lagrangian_particles_event (GfsEvent * event, GfsSimulation * sim) {

  if ((* GFS_EVENT_CLASS (GTS_OBJECT_CLASS (lagrangian_particles_class ())->parent_class)->event) (event, sim)) {
    

    /* do object-specific event here */
    GfsDomain * domain = GFS_DOMAIN (sim);
    GfsLagrangianParticles *lagrangian = LAGRANGIAN_PARTICLES(event);

//
//    if(lagrangian->first_call){
//      lagrangian->first_call = FALSE;
//
//      GSList *i = lagrangian->particles;
//      lagrangian->maxid = 0;
//      while(i){
//	Particle *p = (Particle *)(i->data);
//	lagrangian->maxid = MAX(lagrangian->maxid, p->id);
//	///gfs_domain_locate takes 4 arguments and returns the GfsBox
//	//p->cell = gfs_domain_locate(domain, p->pos, -1);
//	p->cell = gfs_domain_locate(domain, p->pos, -1, NULL);
//	i = i->next;
//	if(!p->cell){
//	  lagrangian->particles = g_slist_remove(lagrangian->particles, p);
//	  g_free(p);
//	}
//      }
//      if(lagrangian->fcoeff.init == 1)
//	init_particles(lagrangian, domain);
//
//      store_domain_previous_vel(domain, lagrangian->un);
//    }
//
//    
//    	GSList *i = lagrangian->particles;
//    	guint nsteps = 1, iter = 0;
//    	Particle * p;
//        GfsVariable ** u = gfs_domain_velocity (domain);
//        FttVector force;
//        gdouble dt = sim->advection_params.dt/(gdouble)nsteps;
//        force_pointer f;
//        GfsSourceDiffusion *d = source_diffusion_viscosity(u[0]);
//    
//        GSList *fs;
//    
//        lagrangian->time = sim->time.t;
//    
//        i = lagrangian->particles;
//        ForceParams * pars = g_malloc(sizeof(ForceParams));    
//        pars->dt = sim->advection_params.dt;
//    
//        pars->fcoeffs = &lagrangian->fcoeff;
//        pars->u = u;
//        pars->un = lagrangian->un;
//        pars->force = &force;
//        pars->lagrangian = lagrangian;
//    
//        reset_couple_force (domain, lagrangian->couplingforce);
//
//	///Not including immersed boundary method    
////        if(lagrangian->fcoeff.imsolid == 1){
////          immersed_solid_motion(lagrangian, pars, d);
////        }
////        else{
//          lagrangian->n = 0;
// 
//     	 while(i){
//
//	p = (Particle *)(i->data);
//	//p->cell = gfs_domain_locate(domain, p->pos, -1);
//	p->cell = gfs_domain_locate(domain, p->pos, -1, NULL);
//	/* 	add_particle_to_hashtable(p, lagrangian->cell_particles, domain); */
//	i = i->next;
//
//	if(!p->cell){
//	  lagrangian->particles = g_slist_remove(lagrangian->particles, p);
//	  g_free(p);
//	}
//	else{
//	  if(lagrangian->fcoeff.fluidadv == 1){
//	    fluidadvect_particles(p, pars->u);
//	  }
//	  else{
//	    gdouble viscosity;
//	    if(d)
//	      viscosity = gfs_diffusion_cell(d->D, p->cell);
//	    else
//	      viscosity = 0.;
// 
//	    gdouble fluid_rho = sim->physical_params.alpha ? 1./
//	      gfs_function_value(sim->physical_params.alpha,p->cell) : 1.;
//	  
//	    p->acc.x = 0.;
//	    p->acc.y = 0.;
//	    p->acc.z = 0.;
//	  
//	    pars->p = p;
//	    pars->fluid_rho = fluid_rho;
//	    pars->viscosity = viscosity;
//	  
//	    fs = lagrangian->forces;
//	    while(fs){
//	      f = (force_pointer)fs->data;
//	      (f)(pars);
//	      compute_particle_acceleration(p, force, lagrangian->fcoeff.cm,
//					    fluid_rho);
//	      fs = fs->next;
//	    }
//	    if(p->move == 1)
//	      compute_particle_velocity (p, dt);
//
//	    lagrangian->maxid = MAX(lagrangian->maxid, p->id);
//	    if(p->cell != NULL){
//	      particles_force(pars);
//	      lagrangian->n++;
//	      /* 	compute_coupling_force2(p, lagrangian->couplingforce, domain); */
//	      compute_coupling_force(p, lagrangian->couplingforce);
//	    }
//	  }	  
//	}	     
//      }
//      	gdouble time = sim->advection_params.dt;
//      	gdouble t = 0, dtmin;
//	
//	///Not including collision for now
//	//Collision *collide = g_malloc0(sizeof(Collision));
//      	while( time > (t + 1.e-12)) {	
//	dtmin = time - t;	
//	
//	///Not including collision for now
////	/* 	  clean_particle_hash_table(lagrangian->cell_particles); */
////	if(lagrangian->fcoeff.collision == 1){
////	  create_particle_hash_table(lagrangian);
////	  
////	  collide->cell_particles = lagrangian->cell_particles;
////	  collide->particles = lagrangian->particles;
////	  collide->dt = dtmin;
////	  collide->colliding = NULL;
////
////	  min_collision_time(collide);
////	  
////	  dtmin = collide->dt;
////	}
//	#ifdef HAVE_MPI
//		gfs_all_reduce(domain, dtmin, MPI_DOUBLE, MPI_MIN);  
//	#endif
//	t += dtmin;
//	if(time < t){
//	  dtmin = time - (t - dtmin);
//	  t = time;
//	}
//	
//	i = lagrangian->particles;
//	while(i){
//	  Particle *p = (Particle *)(i->data);
//	  ///p->cell = gfs_domain_locate(domain, p->pos, -1);
//	  p->cell = gfs_domain_locate(domain, p->pos, -1, NULL);
//	
//	  /*Update particle position and also HashTable*/
//	  if(p->move == 1)
//	    advect_particle(p, dtmin);
//	  /* 	      update_particle_in_hash_table(lagrangian->cell_particles, p, domain, dtmin); */
//	  /*solid_reflection (domain, p, dtmin); */
//	  i = i->next;
//	}
//
////	if(lagrangian->fcoeff.collision == 1){
////	  i = collide->colliding;
////
////	  while(i){
////	    Particle *pi = (Particle *)(i->data);
////	    i = i->next;
////	    if(!i)
////	      break;
////	    Particle *pj = (Particle *)(i->data);
////	    if(lagrangian->fcoeff.merging==1){
////	      Particle *pnew =  merge_particles(pi, pj);
////	      pnew->id = ++lagrangian->n;
////	      lagrangian->particles = g_slist_append(lagrangian->particles, pnew);
////	      lagrangian->particles = g_slist_remove(lagrangian->particles, pi);
////	      lagrangian->particles = g_slist_remove(lagrangian->particles, pj);
////	      g_free(pi);
////	      g_free(pj);
////	    }
////	    else
////	      make_collision(pi, pj);
////	  
////	    i = i->next;
////	  }
////	  g_slist_free(collide->colliding);
////	  clean_particle_hash_table(lagrangian->cell_particles);
////	}
//	/*Applying Boundary Conditions 2 times in 2D and 3 in 3D to take into account particle crossing to a corner process which can't be checked in an easy way*/	 
//	boundary_particles(lagrangian, domain);
//	boundary_particles(lagrangian, domain);
//	
//	#if !FTT_2D
//		boundary_particles(lagrangian, domain);
//	#endif
//	
//      	//}
//      	///Collision not included for now
//	//g_free(collide);
//      	store_domain_previous_vel(domain, lagrangian->un);
//      	/*Clean the HashTable*/
//    	}
//
//    	g_free(pars);
//
//    	/*    FttComponent c; */
//    	/*   for(c = 0; c < FTT_DIMENSION; c++)     */
//    	/*     gfs_domain_cell_traverse  */
//    	/*       (domain, */
//    	/*        FTT_POST_ORDER, FTT_TRAVERSE_NON_LEAFS, -1, */
//    	/*        (FttCellTraverseFunc) (lagrangian->couplingforce)[c]->fine_coarse, */
//    	/*        lagrangian->couplingforce[c]); */
//
//
//	#ifdef HAVE_MPI
//    		gfs_all_reduce(domain, lagrangian->maxid, MPI_INT, MPI_MAX);  
//	#endif
    return TRUE;
  }
  return FALSE;
}


/*Particle data read method*/
static gboolean particle_read (GtsFile * fp, guint * id,
                               FttVector * p, FttVector * v,
                               gdouble *density, gdouble *volume, gint *move, FttVector * q)
{

  if (fp->type != GTS_INT) {
    gts_file_error (fp, "expecting an integer (Id)");
    return FALSE;
  }

  *id = atoi (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (p.x)");
    return FALSE;
  }
  p->x = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (p.y)");
    return FALSE;
  }
  p->y = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (p.z)");
    return FALSE;
  }
  p->z = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (v.x)");
    return FALSE;
  }
  v->x = atof (fp->token->str);
  gts_file_next_token (fp);
  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (v.y)");
    return FALSE;
  }
  v->y = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (v.z)");
    return FALSE;
  }
  v->z = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (density)");
    return FALSE;
  }
  *density = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type != GTS_INT && fp->type != GTS_FLOAT) {
    gts_file_error (fp, "expecting a number (volume)");
    return FALSE;
  }
  *volume = atof (fp->token->str);
  gts_file_next_token (fp);

  if (fp->type == GTS_INT){
    *move = atoi(fp->token->str);
    gts_file_next_token (fp);
  }

  /*Moving Coordinate frame*/
  q->x = 0.;
  q->y = 0.;
  q->z = 0.;
  if (fp->type == GTS_INT || fp->type == GTS_FLOAT) {
    q->x = atof (fp->token->str);
    gts_file_next_token (fp);
  }
  if (fp->type == GTS_INT || fp->type == GTS_FLOAT) {
    q->y = atof (fp->token->str);
    gts_file_next_token (fp);
  }
  if (fp->type == GTS_INT || fp->type == GTS_FLOAT) {
    q->z = atof (fp->token->str);
    gts_file_next_token (fp);
  }

  return TRUE;
}


static void assign_val_vars (guint * f, GtsFile *fp, GtsObject *o) {

  gts_file_next_token (fp);
  if (fp->type != '=') {
    gts_file_error (fp, "expecting `='");
    return;
  }
  gts_file_next_token (fp);

  if (fp->type == GTS_INT)
    *f = atoi(fp->token->str);

  gts_file_next_token (fp);

}

static void assign_val_funcs (GfsFunction * f, GtsFile *fp, GfsLagrangianParticles *o) {

  GfsDomain * domain = GFS_DOMAIN(gfs_object_simulation (o));


  gts_file_next_token (fp);
  if (fp->type != '=') {
    gts_file_error (fp, "expecting `='");
    return;
  }

  gts_file_next_token (fp);
  gfs_function_read (f, domain, fp);

  if (fp->type == GTS_ERROR) {
    gts_file_error (fp, "Trouble reading function for Force Coeffs in GfsLagrangianParticles module");
    gts_object_destroy (GTS_OBJECT (f));
    return;
  }

}

/*Lagrangian particle object read method*/
static void lagrangian_particles_read (GtsObject ** o, GtsFile * fp)
{

  /* call read method of parent */
  if (GTS_OBJECT_CLASS (lagrangian_particles_class ())->parent_class->read)
    (* GTS_OBJECT_CLASS (lagrangian_particles_class ())->parent_class->read) 
      (o, fp);
  if (fp->type == GTS_ERROR)
    return;

  /* do object-specific read here */
  GfsLagrangianParticles * lagrangian = LAGRANGIAN_PARTICLES(*o);

  GfsDomain * domain = GFS_DOMAIN (gfs_object_simulation (lagrangian));

  if (fp->type == GTS_STRING) {  
    lagrangian->name = g_string_new(fp->token->str); 

    lagrangian->couplingforce = g_malloc(sizeof(GfsVariable));
    couple_force_define(domain, lagrangian->couplingforce, lagrangian->name);

    gts_file_next_token (fp);
  }
  else {
    gts_file_error (fp, "expecting a string");
    return;
  }

  if (fp->type == GTS_STRING) {  

    lagrangian->density = gfs_domain_get_or_add_variable (domain, fp->token->str, 
							  "Lagrangian Density variable");
    if(lagrangian->density == NULL){
      gts_file_error (fp, "Expecting a defined variable name for fluid density evaluation.");
      return;
    }

    gts_file_next_token (fp);
  }

  lagrangian->reynolds = gfs_domain_get_or_add_variable (domain, "Rep", 
							 "Particle Reynolds number");

  lagrangian->urel = gfs_domain_get_or_add_variable (domain, "Urelp", 
						     "Particle x - relative velocity");

  lagrangian->vrel = gfs_domain_get_or_add_variable (domain, "Vrelp", 
						     "Particle y - relative velocity");
#if !FTT_2D
  lagrangian->wrel = gfs_domain_get_or_add_variable (domain, "Wrelp", 
						     "Particle z - relative velocity");
#endif

  lagrangian->pdia = gfs_domain_get_or_add_variable (domain, "Pdia", 
						     "Particle radii");

  while (fp->type == '\n')
    gts_file_next_token (fp);

  if (fp->type != '{') {
    gts_file_error (fp, "expecting an opening brace");
    return;
  }
  fp->scope_max++;
  gts_file_next_token (fp);

  while (fp->type != GTS_ERROR && fp->type != '}') {
    if (fp->type == '\n') {
      gts_file_next_token (fp);
      continue;
    }
    if (fp->type != GTS_STRING) {
      gts_file_error (fp, "expecting a variable name");
      return;
    }
    else {

      if(g_ascii_strcasecmp(fp->token->str, "lift") == 0)
	assign_val_vars (&lagrangian->fcoeff.lift, fp, *o);
	
      else if(g_ascii_strcasecmp(fp->token->str, "drag") == 0)
	assign_val_vars (&lagrangian->fcoeff.drag, fp, *o);
	
      else if(g_ascii_strcasecmp(fp->token->str, "buoy") == 0)
	assign_val_vars (&lagrangian->fcoeff.buoy, fp, *o);
	
      else if(g_ascii_strcasecmp(fp->token->str, "inertial") == 0)
	assign_val_vars (&lagrangian->fcoeff.inertial, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "collision") == 0)
	assign_val_vars (&lagrangian->fcoeff.collision, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "merging") == 0){
	assign_val_vars (&lagrangian->fcoeff.merging, fp, *o);
	if(lagrangian->fcoeff.merging==1)
	  lagrangian->fcoeff.collision = 1;
      }
	
      else if(g_ascii_strcasecmp(fp->token->str, "faxen") == 0)
	assign_val_vars (&lagrangian->fcoeff.faxen, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "amf") == 0){
	assign_val_vars (&lagrangian->fcoeff.amf, fp, *o);
	lagrangian->fcoeff.cm = 0.5;
      }

      else if(g_ascii_strcasecmp(fp->token->str, "basset") == 0)
	assign_val_vars (&lagrangian->fcoeff.basset, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "init") == 0)
	assign_val_vars (&lagrangian->fcoeff.init, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "imsolid") == 0)
	assign_val_vars (&lagrangian->fcoeff.imsolid, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "bending") == 0){
	gts_file_next_token (fp);
	if (fp->type != '=') {
	  gts_file_error (fp, "expecting `='");
	  return;
	}
	gts_file_next_token (fp);
	
	if (fp->type == GTS_INT || fp->type == GTS_FLOAT)
	   lagrangian->fcoeff.bending = atof(fp->token->str);
	
	gts_file_next_token (fp);
      }

      else if(g_ascii_strcasecmp(fp->token->str, "tension") == 0){
	gts_file_next_token (fp);
	if (fp->type != '=') {
	  gts_file_error (fp, "expecting `='");
	  return;
	}
	gts_file_next_token (fp);
	
	if (fp->type == GTS_INT || fp->type == GTS_FLOAT)
	  lagrangian->fcoeff.tension = atof(fp->token->str);
	
	gts_file_next_token (fp);
      }

      else if(g_ascii_strcasecmp(fp->token->str, "fluidadv") == 0)
	assign_val_vars (&lagrangian->fcoeff.fluidadv, fp, *o);

      else if(g_ascii_strcasecmp(fp->token->str, "cdrag") == 0){
	lagrangian->fcoeff.cdrag = gfs_function_new (gfs_function_class (), 0.);
	assign_val_funcs (lagrangian->fcoeff.cdrag, fp, lagrangian);
      }

      else if(g_ascii_strcasecmp(fp->token->str, "clift") == 0){
	lagrangian->fcoeff.clift = gfs_function_new (gfs_function_class (), 0.);
	assign_val_funcs (lagrangian->fcoeff.clift, fp, lagrangian);
      }

      else if(g_ascii_strcasecmp(fp->token->str, "camf") == 0){
	lagrangian->fcoeff.camf = gfs_function_new (gfs_function_class (), 0.);
	assign_val_funcs (lagrangian->fcoeff.camf, fp, lagrangian);
      }

      else if(g_ascii_strcasecmp(fp->token->str, "cfaxen") == 0){
	lagrangian->fcoeff.cfaxen = gfs_function_new (gfs_function_class (), 0.);
	assign_val_funcs (lagrangian->fcoeff.cfaxen, fp, lagrangian);
      }
      else{
	gts_file_error (fp, "Not a valid variable");
	return;
      }	
    }
  }

  if (fp->type != '}') {
    gts_file_error (fp, "expecting a closing brace");
    return;
  }

  fp->scope_max--;

  lagrangian->forces = NULL;  
  force_pointer f;

  if(lagrangian->fcoeff.lift == 1){
    f = &compute_lift_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }

  if(lagrangian->fcoeff.drag == 1){
    f = &compute_drag_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }
  
  if(lagrangian->fcoeff.buoy == 1){
    f = &compute_buoyant_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }

  if(lagrangian->fcoeff.inertial == 1){
    f = &compute_inertial_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }

  if(lagrangian->fcoeff.faxen == 1){
    f = &compute_faxen_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }

  if(lagrangian->fcoeff.amf == 1){
    f = &compute_amf_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }

  if(lagrangian->fcoeff.basset == 1){
    f = &compute_basset_force;
    lagrangian->forces = g_slist_append(lagrangian->forces, f);
  }

  if (fp->type == GTS_ERROR)
    return;

  do
    gts_file_next_token (fp);
  while (fp->type == '\n');

  lagrangian->particles = NULL; 
  lagrangian->maxid = 0;

  //printf("check");

  if (fp->type == GTS_STRING) {
 
    FILE *fptr = fopen(fp->token->str, "r");
    GtsFile *fp1;
    if(fptr == NULL) {
      gts_file_error (fp, "cannot open file `%s'", fp->token->str);
      return;
    }
    fp1 = gts_file_new (fptr);

    while (fp1->type == '\n')
      gts_file_next_token (fp1);

    if(fp1->type == GTS_INT)
      lagrangian->n = atoi(fp1->token->str);
    else{
      gts_file_error (fp1, "expecting an integer (n)");
      return;
    }

    gts_file_next_token (fp1);

    if(fp1->type == GTS_FLOAT || fp1->type == GTS_INT )
      lagrangian->time = atof(fp1->token->str);

    else{
      gts_file_error (fp1, "expecting a number (time)");
      return;
    }   

    do
      gts_file_next_token (fp1); 
    while (fp1->type == '\n');  

    while (fp1->type != GTS_NONE) {
      guint id;
      FttVector p,v;
      gdouble density,volume;
      gint move = 1;
      /*IBM Coordinates*/
      FttVector q;   
      if (!particle_read (fp1, &id, &p, &v, &density, &volume, &move, &q)) {
	gts_file_error (fp, "%s:%d:%d: %s", fp->token->str, fp1->line, fp1->pos, fp1->error);
	return;
      }
      Particle * particles = g_malloc(sizeof(Particle));
      particles->id = id;
      particles->pos = p;
      particles->vel = v;
      particles->density = density;
      particles->volume = volume;
      particles->move = move;
      particles->q = q;

      if(id > lagrangian->maxid)
	lagrangian->maxid = id;

      lagrangian->particles = g_slist_append(lagrangian->particles, particles);
      do
	gts_file_next_token (fp1);
      while (fp1->type == '\n');
    }
    gts_file_destroy (fp1);
    fclose (fptr);
    gts_file_next_token (fp);
    
  }
  else if (fp->type == '{') {

    fp->scope_max++;

    do
      gts_file_next_token (fp);
    while (fp->type == '\n');


    if(fp->type == GTS_INT)
      lagrangian->n = atoi(fp->token->str);
    else{
      gts_file_error (fp, "expecting an integer (n)");
      return;
    }
    gts_file_next_token (fp);

    if(fp->type == GTS_FLOAT || fp->type == GTS_INT )
      lagrangian->time = atof(fp->token->str);
    else{
      gts_file_error (fp, "expecting a number (time)");
      return;
    }   

    do
      gts_file_next_token (fp);
    while (fp->type == '\n');

    while (fp->type != GTS_NONE && fp->type != '}') {
      guint id;
      FttVector p,v;
      gdouble density,volume;
      gint move = 1;
      /*IBM Coordinates*/
      FttVector q; 

      if (!particle_read (fp, &id, &p, &v, &density, &volume, &move, &q))
	return;
      Particle * particles=g_malloc(sizeof(Particle));
      particles->id = id;
      particles->pos = p;
      particles->vel = v;
      particles->density = density;
      particles->volume = volume;
      particles->move = move;
      particles->q = q;

      lagrangian->particles = g_slist_append(lagrangian->particles, particles);

      do
	gts_file_next_token (fp);
      while (fp->type == '\n');
    }

    if (fp->type != '}') {
      gts_file_error (fp, "expecting a closing brace");
      return;
    }
    fp->scope_max--;

    gts_file_next_token (fp);
  }

  if (fp->type == GTS_ERROR)
    return;  

  lagrangian->un = g_malloc(sizeof(GfsVariable));
  previous_time_vel(domain, lagrangian->un);
}

/*Lagrangian particle object write method*/
static void lagrangian_particles_write (GtsObject * o, FILE * fp) {
  
	/* call write method of parent */

  	if (GTS_OBJECT_CLASS (lagrangian_particles_class ())->parent_class->write)
    	(* GTS_OBJECT_CLASS (lagrangian_particles_class ())->parent_class->write)
      	(o, fp);

  	/* do object specific write here */
  	GfsLagrangianParticles * lagrangian = LAGRANGIAN_PARTICLES(o);

  	fprintf(fp," %s ",lagrangian->name->str);
  	if(lagrangian->density)
      		fprintf(fp," %s ",lagrangian->density->name);
  		fprintf(fp," {");
  	if(lagrangian->fcoeff.imsolid == 1)
    		fprintf(fp, " imsolid = 1");
  	if(fabs(lagrangian->fcoeff.bending) > 0)
    		fprintf(fp, " bending = %g ", lagrangian->fcoeff.bending);
  	if(fabs(lagrangian->fcoeff.tension) > 0)
    		fprintf(fp, " tension = %g ", lagrangian->fcoeff.tension);
  	if(lagrangian->fcoeff.lift == 1)
    		fprintf(fp, " lift = 1");
  	if(lagrangian->fcoeff.drag == 1)
    		fprintf(fp, " drag = 1");
  	if(lagrangian->fcoeff.buoy == 1)
    		fprintf(fp, " buoy = 1");
  	if(lagrangian->fcoeff.inertial == 1)
    		fprintf(fp, " inertial = 1");
  	if(lagrangian->fcoeff.collision == 1)
    		fprintf(fp, " collision = 1");
  	if(lagrangian->fcoeff.merging == 1)
    		fprintf(fp, " merging = 1");
  	if(lagrangian->fcoeff.faxen == 1)
    		fprintf(fp, " faxen = 1");
  	if(lagrangian->fcoeff.amf == 1)
    		fprintf(fp, " amf = 1");
  	if(lagrangian->fcoeff.basset == 1)
    		fprintf(fp, " basset = 1");
  	if(lagrangian->fcoeff.init == 1)
    		fprintf(fp, " init = 1");
  	if(lagrangian->fcoeff.fluidadv == 1)
    		fprintf(fp, " fluidadv = 1");
  	if(lagrangian->fcoeff.cdrag){
    		fprintf(fp, " cdrag = ");
  	gfs_function_write(lagrangian->fcoeff.cdrag,fp);
  	}
  	if(lagrangian->fcoeff.clift){
    		fprintf(fp, " clift = ");
    		gfs_function_write(lagrangian->fcoeff.clift,fp);
  	}
	if(lagrangian->fcoeff.camf){
    		fprintf(fp, " camf = ");
    		gfs_function_write(lagrangian->fcoeff.camf,fp);
  	}
  	if(lagrangian->fcoeff.cfaxen){
    		fprintf(fp, " cfaxen = ");
    		gfs_function_write(lagrangian->fcoeff.cfaxen,fp);
  	}
  	fprintf (fp," } \n");

  	GSList *i = lagrangian->particles;
  	Particle * p;
  	fputs (" { \n",fp);
  	fprintf(fp,"%d %g\n", lagrangian->n, lagrangian->time);
  	while(i) {
    		p = (Particle *)(i->data);
    		fprintf(fp,"%d %g %g %g %g %g %g %g %g %d ", p->id, p->pos.x, p->pos.y, p->pos.z,
            	p->vel.x, p->vel.y, p->vel.z, p->density, p->volume, p->move);
    		if(lagrangian->fcoeff.imsolid == 1)
      			fprintf(fp,"%g %g %g",p->q.x, p->q.y, p->q.z);
    		fprintf(fp,"\n");
    		i = i->next;
  	}
  	fputs (" } \n",fp);

}

static void lagrangian_particles_destroy (GtsObject * object) {

  /* do not forget to call destroy method of the parent */
  (* GTS_OBJECT_CLASS (lagrangian_particles_class ())->parent_class->destroy)
    (object);
  /* do object-specific cleanup here */
  GfsLagrangianParticles * lagrangian = LAGRANGIAN_PARTICLES(object);



  if(lagrangian->particles)
    g_slist_foreach (lagrangian->particles, (GFunc) g_free, NULL);

  g_slist_free(lagrangian->particles);

  g_slist_free(lagrangian->forces);

  g_string_free(lagrangian->name, TRUE);

  if(lagrangian->fcoeff.cdrag)
    g_free(lagrangian->fcoeff.cdrag);

  if(lagrangian->fcoeff.camf)
    g_free(lagrangian->fcoeff.camf);

  if(lagrangian->fcoeff.clift)
    g_free(lagrangian->fcoeff.clift);

  if(lagrangian->fcoeff.cfaxen)
    g_free(lagrangian->fcoeff.cfaxen);

  if(lagrangian->couplingforce)
    g_free(lagrangian->couplingforce);

/*   if(lagrangian->cell_particles){ */
/*     //    clean_particle_hash_table(lagrangian->cell_particles); */
/*     destroy_particle_hash_table(lagrangian->cell_particles); */
/*   } */
}

static void lagrangian_particles_class_init (GfsLagrangianParticlesClass * klass) {
  /* define new methods and overload inherited methods here */

	printf("Initializing lagrangian particles class \n");
 
	GFS_EVENT_CLASS (klass)->event = lagrangian_particles_event;
  	GTS_OBJECT_CLASS (klass)->read = lagrangian_particles_read;
  	//GTS_OBJECT_CLASS (klass)->write = lagrangian_particles_write;
  	//GTS_OBJECT_CLASS (klass)->destroy = lagrangian_particles_destroy;
 
}

static void lagrangian_particles_init (GfsLagrangianParticles * object) {


  printf("Initializing lagrangian particles object \n");

  /* initialize object here */
  object->fcoeff.cm = 0.;
  object->fcoeff.cl = 0.;
  object->fcoeff.cd = 0.;
  object->fcoeff.cf = 0.;

  object->fcoeff.bending = 0.;
  object->fcoeff.tension = 0.;

  object->fcoeff.init = 0;
  object->fcoeff.fluidadv = 0;
  object->fcoeff.lift = 0;
  object->fcoeff.drag = 0;
  object->fcoeff.buoy = 0;
  object->fcoeff.inertial = 0;
  object->fcoeff.faxen = 0;
  object->fcoeff.amf = 0;
  object->fcoeff.basset = 0;
  object->fcoeff.imsolid = 0;

  object->fcoeff.cdrag = NULL;

  object->n = 0;
  object->forces = NULL;
  object->particles = NULL;

  object->cell_particles = g_hash_table_new(g_direct_hash, g_direct_equal);
  object->first_call = TRUE;
}



GfsLagrangianParticlesClass * lagrangian_particles_class (void) {
static GfsLagrangianParticlesClass * klass = NULL;

  if (klass == NULL) {
    GtsObjectClassInfo lagrangian_particles_info = {
      "GfsLagrangianParticles",
      sizeof (GfsLagrangianParticles),
      sizeof (GfsLagrangianParticlesClass),
      (GtsObjectClassInitFunc) lagrangian_particles_class_init,
      (GtsObjectInitFunc) lagrangian_particles_init,
      (GtsArgSetFunc) NULL,
      (GtsArgGetFunc) NULL
    };
    klass = gts_object_class_new (GTS_OBJECT_CLASS (gfs_event_class ()),
                                  &lagrangian_particles_info);
  }

  return klass;
}


/*GfsSourceGfsLagrangianParticles: Object*/

static void source_lagrangian_destroy (GtsObject * o)
{

  g_string_free(GFS_SOURCE_LAGRANGIAN(o)->name,TRUE);

  (* GTS_OBJECT_CLASS (gfs_source_lagrangian_class ())->parent_class->destroy) (o);

}

static void source_lagrangian_read (GtsObject ** o, GtsFile * fp)
{

  if (GTS_OBJECT_CLASS (gfs_source_diffusion_explicit_class ())->parent_class->read)
    (* GTS_OBJECT_CLASS (gfs_source_lagrangian_class ())->parent_class->read) 
      (o, fp);
 
  if (fp->type == GTS_ERROR)
    return;
 
  GfsSourceLagrangian * source = GFS_SOURCE_LAGRANGIAN(*o);

  if (fp->type == GTS_STRING) {  
    source->name = g_string_new(fp->token->str); 
    gts_file_next_token (fp);
  }
  else 
    gts_file_error (fp, "expecting a string");
 
}

static void source_lagrangian_write (GtsObject * o, FILE * fp)
{
  (* GTS_OBJECT_CLASS (gfs_source_lagrangian_class ())->parent_class->write) (o, fp);

  GfsSourceLagrangian * source = GFS_SOURCE_LAGRANGIAN(o);

  fprintf(fp," %s\n",source->name->str);
}


/* Add source term in the Momentum equations*/
static gdouble compute_phi_force_mac_value(GfsSourceGeneric * s,
					   FttCell * cell,
					   GfsVariable * v)
{

  GfsDomain *d = GFS_DOMAIN(gfs_object_simulation(s));
  GfsVariable *force;
  gchar * name;
  GfsSourceLagrangian *sv = GFS_SOURCE_LAGRANGIAN(s);
  
  FttCellFace f;
  f.cell = cell;

  FttCellNeighbors n;
  ftt_cell_neighbors (cell, &n);

  switch(v->component){
  case FTT_X: 
    name = g_strconcat(sv->name->str, "x", NULL);
    force = gfs_variable_from_name(d->variables, name);
    f.d = FTT_RIGHT;
    f.neighbor = n.c[f.d];
    break;
  case FTT_Y: 
    name = g_strconcat(sv->name->str, "y", NULL);
    force = gfs_variable_from_name(d->variables, name);
    f.d = FTT_TOP;
    f.neighbor = n.c[f.d];
    break;
#if !FTT_2D
  case FTT_Z: 
    name = g_strconcat(sv->name->str, "z", NULL);
    force = gfs_variable_from_name(d->variables, name);
    f.d = FTT_FRONT;
    f.neighbor = n.c[f.d];
    break;
#endif
  default: g_assert_not_reached ();
  }

  g_free(name);
  if(force!=NULL)
    return  gfs_face_interpolated_value(&f, force->i);
  else
    g_assert_not_reached ();

  return 0.;

}

static gdouble compute_phi_force_centered_value(GfsSourceGeneric * s,
						FttCell * cell,
						GfsVariable * v)
{

  GfsDomain *d = GFS_DOMAIN(gfs_object_simulation(s));
  GfsVariable *force;
  gchar * name;
  GfsSourceLagrangian *sv = GFS_SOURCE_LAGRANGIAN(s);

  switch(v->component){
  case FTT_X: 
    name = g_strconcat(sv->name->str, "x", NULL);
    force = gfs_variable_from_name(d->variables, name);
    break;
  case FTT_Y:
    name = g_strconcat(sv->name->str, "y", NULL);
    force = gfs_variable_from_name(d->variables, name);
    break;
#if !FTT_2D
  case FTT_Z:
    name = g_strconcat(sv->name->str, "z", NULL);
    force = gfs_variable_from_name(d->variables, name);
    break;
#endif
  default: g_assert_not_reached ();
  }

  g_free(name);
  if(force!=NULL){
    ///return GFS_VARIABLE(cell,force->i);}
    return GFS_VALUE(cell,force);}
  else
    g_assert_not_reached ();

}

static void gfs_source_lagrangian_class_init (GfsSourceGenericClass * klass)
{

  GTS_OBJECT_CLASS (klass)->destroy = source_lagrangian_destroy;
  GTS_OBJECT_CLASS (klass)->read = source_lagrangian_read;
  GTS_OBJECT_CLASS (klass)->write = source_lagrangian_write;

}

static void gfs_source_lagrangian_init (GfsSourceGeneric * s)
{
  s->mac_value= compute_phi_force_mac_value;
  s->centered_value = compute_phi_force_centered_value;
}

GfsSourceGenericClass * gfs_source_lagrangian_class (void)
{
  static GfsSourceGenericClass * klass = NULL;

  if (klass == NULL) {
    GtsObjectClassInfo gfs_source_lagrangian_info = {
      "GfsSourceLagrangian",
      sizeof (GfsSourceLagrangian),
      sizeof (GfsSourceGenericClass),
      (GtsObjectClassInitFunc) gfs_source_lagrangian_class_init,
      (GtsObjectInitFunc) gfs_source_lagrangian_init,
      (GtsArgSetFunc) NULL,
      (GtsArgGetFunc) NULL
    };
    klass = gts_object_class_new (GTS_OBJECT_CLASS (gfs_source_velocity_class ()),
				  &gfs_source_lagrangian_info);
  }

  return klass;
}

/*GfsFeedParticles: Object*/
static void feed_particles_destroy (GtsObject * o)
{
  GfsFeedParticles * feedparticles = FEED_PARTICLES(o);
  
  g_slist_foreach (feedparticles->particles, (GFunc) g_free, NULL);
  g_slist_free(feedparticles->particles);

  (* GTS_OBJECT_CLASS (feed_particles_class ())->parent_class->destroy) (o);
}

static void feed_particles_read (GtsObject ** o, GtsFile * fp)
{

  (* GTS_OBJECT_CLASS (feed_particles_class ())->parent_class->read) (o, fp);

  if (fp->type == GTS_ERROR)
    return;

  GfsFeedParticles * feedparticles = FEED_PARTICLES(*o);

  while (fp->type == '\n')
    gts_file_next_token (fp);

  if (fp->type == GTS_FLOAT) {  
    feedparticles->feed = atof(fp->token->str); 
    gts_file_next_token (fp);
  }
  else {
    gts_file_error (fp, "expecting a float");
    return;
  }


  feedparticles->particles = NULL;

  while (fp->type == '\n')
    gts_file_next_token (fp); 

  if (fp->type == GTS_STRING) {

    FILE *fptr = fopen(fp->token->str, "r");

    GtsFile * fp1;

    if(fptr == NULL) {
      gts_file_error (fp, "cannot open file `%s'", fp->token->str);
      return;
    }

    fp1 = gts_file_new (fptr);

    while (fp1->type != GTS_NONE) {
 
      guint id;
      FttVector p,v;
      gdouble density,volume;
      gint move = 1;
      FttVector q;
      if (!particle_read (fp1, &id, &p, &v, &density, &volume, &move, &q)) {
	gts_file_error (fp, "%s:%d:%d: %s", fp->token->str, fp1->line, fp1->pos, fp1->error);
	return;
      }
 
      Particle * particles = g_malloc(sizeof(Particle));
      particles->id = id;
      particles->pos = p;
      particles->vel = v;
      particles->density = density;
      particles->volume = volume;
      particles->move = move;
      particles->q = q;
      feedparticles->particles = g_slist_append(feedparticles->particles, particles);

      while (fp1->type == '\n')
	gts_file_next_token (fp1);   
    }
    gts_file_destroy (fp1);
    fclose (fptr);
    gts_file_next_token (fp);
     
  }
  else if (fp->type == '{') {

    fp->scope_max++;
    do
      gts_file_next_token (fp);
    while (fp->type == '\n');

    while (fp->type != GTS_NONE && fp->type != '}') {
      guint id;
      FttVector p,v;
      gdouble density, volume;
      gint move = 1;
      FttVector q;
      if (!particle_read (fp, &id, &p, &v, &density, &volume, &move, &q))
	return;
      Particle * particles=g_malloc(sizeof(Particle));
      particles->id = id;
      particles->pos = p;
      particles->vel = v;
      particles->density = density;
      particles->volume = volume;
      particles->move = move;
      particles->q = q;

      feedparticles->particles = g_slist_append(feedparticles->particles, particles);

      do
	gts_file_next_token (fp); 
      while (fp->type == '\n'); 

 
    }

    if (fp->type != '}') {
      gts_file_error (fp, "expecting a closing brace");
      return;
    }
    fp->scope_max--;

    gts_file_next_token (fp);
  }

  if (fp->type == GTS_ERROR)
    return;

  feedparticles->time = gfs_object_simulation(*o)->time.t - 
    ((gint) (gfs_object_simulation(*o)->time.t/feedparticles->feed))*feedparticles->feed;

  if(feedparticles->time ==0)
    feedparticles->time = feedparticles->feed;

}

static void feed_particles_write (GtsObject * o, FILE * fp)
{
  (* GTS_OBJECT_CLASS (feed_particles_class ())->parent_class->write) (o, fp);

  GfsFeedParticles * feedparticles = FEED_PARTICLES(o);

  fprintf(fp," %lf\n",feedparticles->feed);

  GSList *i = feedparticles->particles;
  Particle * p;
  fputs (" {\n",fp);
  while(i){
    p = (Particle *)(i->data);
    fprintf(fp,"%d %g %g %g %g %g %g %g %g\n", p->id, p->pos.x, p->pos.y, p->pos.z,
            p->vel.x, p->vel.y, p->vel.z, p->density, p->volume);
    i = i->next;
  }
  fputs (" }\n",fp);
}


static gboolean feed_particles_event (GfsEvent * event, GfsSimulation * sim)
{
 
  if ((* GFS_EVENT_CLASS (GTS_OBJECT_CLASS (feed_particles_class ())->parent_class)->event) 
      (event, sim)) {
    /* do object-specific event here */

    GfsDomain * domain = GFS_DOMAIN (sim);
    GfsLagrangianParticles *lagrangian = LAGRANGIAN_PARTICLES(event);
    GfsFeedParticles *feedparticles = FEED_PARTICLES(event);

 
    if(feedparticles->time >= feedparticles->feed){

      GSList *i = lagrangian->particles;
/*       guint idlast = 0; */
      
/*       while(i){ */
/* 	Particle *p = (Particle *)(i->data); */
/* 	idlast = p->id; */
/* 	i = i->next; */
/*       } */

      lagrangian->idlast = lagrangian->maxid;
      
      GSList *j = feedparticles->particles;
      
      while(j){

	Particle *p = (Particle *)(j->data); 
	Particle *p1 = g_malloc(sizeof(Particle));

	memcpy(p1,p,sizeof(Particle));

	p1->id = ++lagrangian->idlast;

	p1->pos.x += p1->vel.x*(feedparticles->time - feedparticles->feed);
	p1->pos.y += p1->vel.y*(feedparticles->time - feedparticles->feed);
	p1->pos.z += p1->vel.z*(feedparticles->time - feedparticles->feed);
	//p1->cell = gfs_domain_locate(domain, p1->pos, -1);
	p1->cell = gfs_domain_locate(domain, p1->pos, -1, NULL);
	if(p1->cell)
	  lagrangian->particles = g_slist_append(lagrangian->particles, p1);

	j = j->next;
      }
      feedparticles->time -= feedparticles->feed;
      
    }
    feedparticles->time += sim->advection_params.dt;
#ifdef HAVE_MPI      
    mpi_particle_numbering(domain, lagrangian);
#endif /*HAVE_MPI*/
    return TRUE;
  }
  return FALSE;
}

static void feed_particles_class_init (GfsFeedParticlesClass * klass)
{
  /* define new methods and overload inherited methods here */

  GFS_EVENT_CLASS (klass)->event = feed_particles_event;
  GTS_OBJECT_CLASS (klass)->read = feed_particles_read;
  GTS_OBJECT_CLASS (klass)->write = feed_particles_write;
  GTS_OBJECT_CLASS (klass)->destroy = feed_particles_destroy;

}

static void feed_particles_init (GfsFeedParticles * object)
{
  object->feed = 0.;
  object->time = 0.;
}

GfsFeedParticlesClass * feed_particles_class (void)
{
  static GfsFeedParticlesClass * klass = NULL;

  if (klass == NULL) {
    GtsObjectClassInfo feed_particles_info = {
      "GfsFeedParticles",
      sizeof (GfsFeedParticles),
      sizeof (GfsFeedParticlesClass),
      (GtsObjectClassInitFunc) feed_particles_class_init,
      (GtsObjectInitFunc) feed_particles_init,
      (GtsArgSetFunc) NULL,
      (GtsArgGetFunc) NULL
    };
    klass = gts_object_class_new (GTS_OBJECT_CLASS (lagrangian_particles_class ()),
				  &feed_particles_info);
  }

  return klass;
}



/*GfsOutputParticle: OutputParticle*/

static void gfs_output_lparticle_read (GtsObject ** o, GtsFile * fp)
{


  (* GTS_OBJECT_CLASS (gfs_output_lparticle_class ())->parent_class->read) (o, fp);

  if (fp->type == GTS_ERROR)
    return;

  GfsOutputLParticle * outputp = GFS_OUTPUT_LPARTICLE(*o);
  
  if (fp->type == GTS_STRING) {  
    outputp->name = g_string_new(fp->token->str); 
    gts_file_next_token (fp);
  }
  else 
    gts_file_error (fp, "expecting a string");

  GfsSimulation *sim = gfs_object_simulation(*o);
  GSList *i = sim->events->items;

  while (i) {
    if(IS_LAGRANGIAN_PARTICLES (i->data)){
      GfsLagrangianParticles *lagrangian = i->data;

      if(g_string_equal(lagrangian->name, outputp->name)){
	outputp->lagrangian = lagrangian;
	break;
      }
    }
    i = i->next;
  }
  
}
static gboolean gfs_output_lparticle_event (GfsEvent * event, 
					      GfsSimulation * sim)
{
  if ((* GFS_EVENT_CLASS (GTS_OBJECT_CLASS (gfs_output_lparticle_class ())->parent_class)->event)
      (event, sim) &&
      sim->advection_params.dt > 0.) {
    GfsDomain * domain = GFS_DOMAIN (sim);
    FILE * fp = GFS_OUTPUT (event)->file->fp;
    GfsOutputLParticle* outputp = GFS_OUTPUT_LPARTICLE(event);


    if (GFS_OUTPUT (event)->first_call)  {
      fputs ("# 1:N 2:T\n", fp);
      fputs ("# 1:ID 2:X 3:Y 4:Z 5:Up 6:Vp 7:Wp 8:rho_p 9:volume_p", fp);
      fputc ('\n', fp);
    }
 
    GfsLagrangianParticles * lagrangian = outputp->lagrangian;
 
    fprintf(fp,"%d %g\n", lagrangian->n, lagrangian->time);
 
    GSList *i = lagrangian->particles;   

    while(i){
      Particle *p = (Particle *)(i->data);
      fprintf(fp,"%d %g %g %g %g %g %g %g %g\n", p->id, p->pos.x, p->pos.y, p->pos.z,
	      p->vel.x, p->vel.y, p->vel.z, p->density, p->volume);
      
      if(p->id > lagrangian->maxid)
	lagrangian->maxid = p->id;
      i = i->next;
    }
 
    fflush(fp);

    return TRUE;
  }
  return FALSE;
}

static void gfs_output_lparticle_write (GfsOutputLParticle * o, GtsFile * fp)
{
  (* GTS_OBJECT_CLASS (gfs_output_lparticle_class ())->parent_class->write) (o, fp);

  GfsOutputLParticle * outputp = GFS_OUTPUT_LPARTICLE(o);

  fprintf(fp," %s\n",outputp->name->str);
}

static void gfs_output_lparticle_destroy (GfsOutputLParticle * o)
{
  g_string_free(GFS_OUTPUT_LPARTICLE(o)->name, TRUE);
  (* GTS_OBJECT_CLASS (gfs_output_lparticle_class ())->parent_class->destroy) (o);
}

static void gfs_output_lparticle_class_init (GfsOutputClass * klass)
{
  GFS_EVENT_CLASS (klass)->event = gfs_output_lparticle_event;
  GTS_OBJECT_CLASS (klass)->write = gfs_output_lparticle_write;
  GTS_OBJECT_CLASS (klass)->read = gfs_output_lparticle_read;
  GTS_OBJECT_CLASS (klass)->destroy = gfs_output_lparticle_destroy;
}

GfsOutputClass * gfs_output_lparticle_class (void)
{
  static GfsOutputClass * klass = NULL;

  if (klass == NULL) {
    GtsObjectClassInfo gfs_output_lparticle_info = {
      "GfsOutputLParticle",
      sizeof (GfsOutputLParticle),
      sizeof (GfsOutputClass),
      (GtsObjectClassInitFunc) gfs_output_lparticle_class_init,
      (GtsObjectInitFunc) NULL,
      (GtsArgSetFunc) NULL,
      (GtsArgGetFunc) NULL
    };
    klass = gts_object_class_new (GTS_OBJECT_CLASS (gfs_output_class ()),
				  &gfs_output_lparticle_info);
  }

  return klass;
}


/* Initialize module*/
const gchar gfs_module_name[] = "lagrangian";
const gchar * g_module_check_init (void);
 
const gchar * g_module_check_init (void)
{
 
   //printf("check");
   lagrangian_particles_class (); 
   //gfs_source_lagrangian_class (); 
   //feed_particles_class (); 
   //gfs_output_lparticle_class (); 


	return NULL;
}

