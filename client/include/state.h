class State {
    virtual void handlevents() = 0;
    virtual void handlelogic() = 0;
    virtual void handledisplay() = 0;
    
    virtual ~State() = default;
};