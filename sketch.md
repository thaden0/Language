namespace atlantis.controllers {
  class UserController {
    private readonly IUserService userService;
    
    new UserController(IUserService userService) this.userService = userService;
    
    @Route('/login', get, noauth)
    LoginResponse login(LoginRequest login) {
        
    }
    
  }
}

@injectable is a DI meta-tag that auto-binds
DbTracking is class that is a DB model

/.env
/trident.toml
/main.lev

/app/
  /controllers/
  /models/
    /dtos/
      /requests/
      /responses/
    /entities/  
  
/config/
  /routes.lev
  /ioc.lev
  /database.lev

/middleware
